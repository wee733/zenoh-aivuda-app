#include "zenoh_bridge/bridge_node.hpp"

#include <chrono>
#include <condition_variable>
#include <thread>

namespace zenoh_bridge {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

constexpr size_t max_payload_bytes = 32 * 1024;
constexpr size_t max_pending_keys = 128;
constexpr size_t max_stream_expressions = 16;
constexpr auto frame_interval = 33ms;

struct StreamQueue {
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::string, std::string> latest;

  void receive(const zenoh::Sample &sample) {
    if (sample.get_kind() != Z_SAMPLE_KIND_PUT || sample.get_payload().size() > max_payload_bytes) return;
    const auto view = sample.get_keyexpr().as_string_view();
    if (view.size() > 256) return;
    const std::string key(view);
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (latest.size() >= max_pending_keys && !latest.count(key)) return;
      latest.insert_or_assign(key, sample.get_payload().as_string());
    }
    changed.notify_one();
  }

  void forward(const HttpResponse::WriteChunk &write, const std::function<bool()> &active) {
    while (active()) {
      decltype(latest) samples;
      {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, 500ms, [&] { return !latest.empty(); });
        latest.swap(samples);
      }
      if (samples.empty()) {
        if (!write(": keepalive\n\n")) return;
        continue;
      }
      for (const auto &[key, payload] : samples) {
        auto json = Json::parse(payload, nullptr, false);
        if (!json.is_object()) continue;
        const Json frame{{"key_expr", key}, {"payload", std::move(json)}};
        if (!write("data: " + frame.dump() + "\n\n")) return;
      }
      // A slow consumer receives the most recent value for each key. Zenoh callbacks
      // never wait for the socket, and fast producers cannot exceed ~30 frames/key/s.
      std::this_thread::sleep_for(frame_interval);
    }
  }
};

HttpResponse error(int code, const std::string &message) {
  return {code, "application/json; charset=utf-8", Json{{"ok", false}, {"message", message}}.dump()};
}

std::vector<zenoh::KeyExpr> parseSelectors(const std::string &selectors) {
  if (selectors.empty() || selectors.size() > 2048 || selectors.back() == ',') {
    throw std::invalid_argument("key_expr must be a comma-separated list of telemetry keys");
  }
  std::vector<zenoh::KeyExpr> expressions;
  std::istringstream input(selectors);
  std::string entry;
  while (std::getline(input, entry, ',')) {
    if (entry.empty() || entry.size() > 256 || expressions.size() == max_stream_expressions) {
      throw std::invalid_argument("Too many or malformed telemetry key expressions");
    }
    zenoh::KeyExpr expr(entry, false);
    if (!zenoh::KeyExpr("*/heartbeat").includes(expr) &&
        !zenoh::KeyExpr("*/comm_status").includes(expr) &&
        !zenoh::KeyExpr("*/telemetry/**").includes(expr)) {
      throw std::invalid_argument("Supported roots: */heartbeat, */comm_status and */telemetry/**");
    }
    if (std::any_of(expressions.begin(), expressions.end(), [&](const auto &other) {
          return other.intersects(expr);
        })) throw std::invalid_argument("Overlapping telemetry expressions");
    expressions.push_back(std::move(expr));
  }
  return expressions;
}

}  // namespace

HttpResponse ZenohBridgeNode::openTelemetryStream(const std::string &selectors) {
  std::vector<zenoh::KeyExpr> expressions;
  try {
    expressions = parseSelectors(selectors);
  } catch (const std::exception &e) {
    return error(400, e.what());
  }
  const auto session = configSnapshot()->active_session;
  if (!session) return error(503, "Zenoh session unavailable");
  auto subscribers = std::make_shared<std::vector<zenoh::Subscriber<void>>>();
  {
    std::lock_guard<std::mutex> lock(stream_slots_mutex_);
    auto slot = std::find_if(stream_slots_.begin(), stream_slots_.end(), [](auto &s) { return s.expired(); });
    if (slot == stream_slots_.end()) return error(503, "Telemetry stream connection limit reached");
    *slot = subscribers;
  }
  auto queue = std::make_shared<StreamQueue>();
  try {
    for (auto &expr : expressions) {
      subscribers->push_back(session->declare_subscriber(expr, [queue](const zenoh::Sample &sample) {
        queue->receive(sample);
      }, zenoh::closures::none));
    }
  } catch (const std::exception &e) {
    return error(503, e.what());
  }
  HttpResponse response;
  response.content_type = "text/event-stream";
  response.stream = [this, session, subscribers, queue](const HttpResponse::WriteChunk &write) {
    if (!write("retry: 1000\n\n")) return;
    queue->forward(write, [&] {
      return http_server_.isRunning() && configSnapshot()->active_session == session;
    });
  };
  return response;
}

void ZenohBridgeNode::observeHeartbeat(const zenoh::Sample &sample) {
  if (sample.get_kind() != Z_SAMPLE_KIND_PUT || sample.get_payload().size() > 4096) return;
  const auto key = sample.get_keyexpr().as_string_view();
  if (key.size() > 256) return;
  const auto id = std::string(key.substr(0, key.find('/')));
  if (id == configSnapshot()->agent_id) return;
  const auto data = Json::parse(sample.get_payload().as_string(), nullptr, false);
  if (!data.is_object()) return;
  const auto host = data.find("avahi_hostname");
  const auto type = data.find("robot_type");
  const auto online = data.find("online");
  std::lock_guard<std::mutex> lock(neighbors_mutex_);
  if (online != data.end() && *online == false) {
    neighbors_.erase(id);
    return;
  }
  if (neighbors_.size() >= 1024 && !neighbors_.count(id)) return;
  neighbors_[id] = {
      host != data.end() && host->is_string() ? host->get<std::string>() : "",
      type != data.end() && type->is_string() ? type->get<std::string>() : "",
      nowSteadyNs(), nowEpochMs()};
}

void ZenohBridgeNode::publishPresence() {
  const auto config = configSnapshot();
  if (!config->active_session) return;
  const auto &id = config->agent_id;
  const auto time_ms = nowEpochMs();
  auto publish = [&](const std::string &key, const Json &payload) {
    auto options = zenoh::Session::PutOptions::create_default();
    options.encoding = zenoh::Encoding::Predefined::application_json();
    options.congestion_control = Z_CONGESTION_CONTROL_DROP;
    config->active_session->put(zenoh::KeyExpr(key), zenoh::Bytes(payload.dump()), std::move(options));
  };
  try {
    if (heartbeat_enabled_) {
      publish(id + "/heartbeat", {{"agent_id", id}, {"key_prefix", id}, {"robot_type", robot_type_},
              {"avahi_hostname", avahi_hostname_}, {"timestamp_unix_ms", time_ms}, {"online", true}});
    }
    if (!comm_status_enabled_) return;
    auto peers = Json::array();
    {
      const auto now = nowSteadyNs();
      std::lock_guard<std::mutex> lock(neighbors_mutex_);
      for (auto it = neighbors_.begin(); it != neighbors_.end();) {
        const auto &[peer, neighbor] = *it;
        if (now - neighbor.received_ns > 2500000000LL || peer == id) {
          it = neighbors_.erase(it);
        } else {
          peers.push_back({{"key", peer + "/heartbeat"}, {"key_prefix", peer}, {"robot_type", neighbor.robot_type},
                           {"avahi_hostname", neighbor.hostname}, {"last_seen_unix_ms", neighbor.received_ms}});
          ++it;
        }
      }
    }
    publish(id + "/comm_status", {{"timestamp_unix_ms", time_ms}, {"self", {
              {"key_prefix", id}, {"key", id + "/heartbeat"}, {"robot_type", robot_type_},
              {"avahi_hostname", avahi_hostname_}}}, {"connected_neighbors", std::move(peers)}});
  } catch (const std::exception &e) {
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 10000, "Presence publish failed: %s", e.what());
  }
}

}  // namespace zenoh_bridge
