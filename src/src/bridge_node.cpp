#include "zenoh_bridge/bridge_node.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>

namespace zenoh_bridge {

namespace {

bool isInternalSystemTopic(const std::string &topic) {
  return topic == "/rosout" || topic == "/parameter_events" ||
         topic.rfind("/rosout/", 0) == 0 ||
         topic.rfind("/parameter_events/", 0) == 0;
}

std::string jsonArrayString(const std::vector<std::string> &items) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &item : items) {
    if (!item.empty()) {
      arr.push_back(item);
    }
  }
  return arr.dump();
}

std::string getSystemHostname() {
  char buf[256] = {0};
  if (::gethostname(buf, sizeof(buf) - 1) == 0) {
    return std::string(buf);
  }
  return "localhost";
}

std::string trimWhitespace(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string reliabilityToStr(rclcpp::ReliabilityPolicy r) {
  switch (r) {
    case rclcpp::ReliabilityPolicy::Reliable: return "reliable";
    case rclcpp::ReliabilityPolicy::BestEffort: return "best_effort";
    default: return "unknown";
  }
}

std::string durabilityToStr(rclcpp::DurabilityPolicy d) {
  switch (d) {
    case rclcpp::DurabilityPolicy::TransientLocal: return "transient_local";
    case rclcpp::DurabilityPolicy::Volatile: return "volatile";
    default: return "unknown";
  }
}

void tryInsertJson5(zenoh::Config &cfg,
                    const std::string &key,
                    const std::string &json5_val,
                    const rclcpp::Logger &logger) {
  try {
    cfg.insert_json5(key, json5_val);
  } catch (const std::exception &e) {
    RCLCPP_WARN(logger, "Zenoh config key '%s' skipped (%s)", key.c_str(), e.what());
  }
}

void strictInsertJson5(zenoh::Config &cfg,
                       const std::string &key,
                       const std::string &json5_val) {
  try {
    cfg.insert_json5(key, json5_val);
  } catch (const std::exception &e) {
    throw std::runtime_error("Invalid Zenoh JSON5 for '" + key + "': " + e.what());
  }
}

void mergeJsonDiffIntoZenohConfig(zenoh::Config &cfg,
                                  const nlohmann::json &def_node,
                                  const nlohmann::json &ovr_node,
                                  const std::string &prefix) {
  if (ovr_node == def_node) {
    return;
  }
  if (ovr_node.is_object() && def_node.is_object() &&
      prefix != "plugins" && prefix != "metadata") {
    for (auto it = ovr_node.begin(); it != ovr_node.end(); ++it) {
      const std::string key = prefix.empty() ? it.key() : (prefix + "/" + it.key());
      if (def_node.contains(it.key())) {
        mergeJsonDiffIntoZenohConfig(cfg, def_node.at(it.key()), it.value(), key);
      } else {
        strictInsertJson5(cfg, key, it.value().dump());
      }
    }
    return;
  }
  if (!prefix.empty()) {
    strictInsertJson5(cfg, prefix, ovr_node.dump());
  }
}

void atomicWriteFile(const std::string &path, const std::string &content) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream ofs(tmp, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
      throw std::runtime_error("Failed to open temp file for writing: " + tmp);
    }
    ofs << content;
    ofs.flush();
    if (!ofs.good()) {
      ofs.close();
      std::remove(tmp.c_str());
      throw std::runtime_error("Failed writing file: " + tmp);
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    throw std::runtime_error("Failed to atomically replace file: " + path);
  }
}

std::vector<std::string> copyHelloLocatorsSafe(const zenoh::Hello &hello) {
  std::vector<std::string> out_vec;
  ::z_owned_string_array_t arr;
  ::z_hello_locators(zenoh::interop::as_loaned_c_ptr(hello), &arr);
  auto loaned = ::z_loan(arr);
  const size_t n = ::z_string_array_len(loaned);
  out_vec.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    auto s = ::z_string_array_get(loaned, i);
    out_vec.emplace_back(reinterpret_cast<const char *>(::z_string_data(s)), ::z_string_len(s));
  }
  ::z_drop(::z_move(arr));
  return out_vec;
}

}  // namespace

ZenohBridgeNode::ZenohBridgeNode(const rclcpp::NodeOptions &options)
    : rclcpp::Node("zenoh_bridge", options) {
  started_epoch_ms_ = nowEpochMs();
  last_stats_ns_ = nowSteadyNs();
  hostname_ = getSystemHostname();

  const char *env_distro = std::getenv("ROS_DISTRO");
  const std::string default_distro =
      (env_distro != nullptr && *env_distro != '\0') ? std::string(env_distro) : std::string("ros2");

  runtime_config_file_ = this->declare_parameter<std::string>("runtime_config_file", "");
  agent_id_ = trimSlashes(this->declare_parameter<std::string>("agent_id", "uav1"));
  default_agent_id_ = agent_id_;
  robot_type_ = this->declare_parameter<std::string>("robot_type", "rotor");
  domain_prefix_ = trimSlashes(this->declare_parameter<std::string>("domain_prefix", kDefaultDomainPrefix));
  ros_distro_ = this->declare_parameter<std::string>("ros_distro", default_distro);
  rules_file_ = this->declare_parameter<std::string>("rules_file", "");
  session_override_file_ = this->declare_parameter<std::string>("session_override_file", "");
  bridge_settings_file_ = this->declare_parameter<std::string>("bridge_settings_file", "");
  default_preset_ = this->declare_parameter<std::string>("default_preset", "empty");
  api_bind_host_ = this->declare_parameter<std::string>("api_bind_host", "127.0.0.1");
  api_port_ = this->declare_parameter<int>("api_port", 17447);
  allow_unprefixed_local_ = this->declare_parameter<bool>("allow_unprefixed_local", true);
  liveliness_enabled_ = this->declare_parameter<bool>("liveliness_enabled", true);

  loadRuntimeConfigFile();

  if (default_agent_id_.empty()) default_agent_id_ = hostname_;
  if (agent_id_.empty()) agent_id_ = default_agent_id_;
  if (domain_prefix_.empty()) domain_prefix_ = kDefaultDomainPrefix;

  initZenohSession();
  loadInitialRules();

  discovery_timer_ = this->create_wall_timer(
      std::chrono::milliseconds(500), [this]() { onDiscoveryTimer(); });
  status_timer_ = this->create_wall_timer(
      std::chrono::milliseconds(1000), [this]() { onStatusTimer(); });

  if (api_port_ > 0) {
    if (http_server_.start(api_bind_host_, api_port_,
                           [this](const HttpRequest &req) { return handleHttpRequest(req); })) {
      RCLCPP_INFO(this->get_logger(), "Zenoh Web API listening on http://%s:%d",
                  api_bind_host_.c_str(), api_port_);
    } else {
      RCLCPP_ERROR(this->get_logger(), "Failed to bind Zenoh Web API on %s:%d",
                   api_bind_host_.c_str(), api_port_);
    }
  }

  RCLCPP_INFO(this->get_logger(),
              "Zenoh Bridge v2.1 started: agent_id=%s, prefix=%s, mode=%s, cfg_mode=%s, rules=%s",
              agent_id_.c_str(), domain_prefix_.c_str(), session_cfg_.mode.c_str(),
              session_cfg_.config_mode.c_str(),
              rules_file_.empty() ? "(memory)" : rules_file_.c_str());
  addLog("INFO", "Zenoh Bridge v2.1 started: agent_id=" + agent_id_ + ", prefix=" + domain_prefix_ +
                     ", mode=" + session_cfg_.mode + ", ros=" + ros_distro_);
}

ZenohBridgeNode::~ZenohBridgeNode() {
  shutting_down_.store(true, std::memory_order_release);
  http_server_.stop();
  if (discovery_timer_) discovery_timer_->cancel();
  if (status_timer_) status_timer_->cancel();
  teardownZenohSession();
}

void ZenohBridgeNode::loadRuntimeConfigFile() {
  if (!runtime_config_file_.empty() && std::filesystem::exists(runtime_config_file_)) {
    try {
      std::ifstream ifs(runtime_config_file_);
      nlohmann::json j;
      ifs >> j;
      if (j.contains("default_agent_id") && j["default_agent_id"].is_string() &&
          !j["default_agent_id"].get<std::string>().empty()) {
        default_agent_id_ = trimSlashes(j["default_agent_id"].get<std::string>());
      }
      if (j.contains("agent_id") && j["agent_id"].is_string() && !j["agent_id"].get<std::string>().empty()) {
        agent_id_ = trimSlashes(j["agent_id"].get<std::string>());
      }
      if (j.contains("robot_type") && j["robot_type"].is_string()) {
        robot_type_ = j["robot_type"].get<std::string>();
      }
      if (j.contains("domain_prefix") && j["domain_prefix"].is_string() &&
          !j["domain_prefix"].get<std::string>().empty()) {
        domain_prefix_ = trimSlashes(j["domain_prefix"].get<std::string>());
      }
      if (j.contains("rules_file") && j["rules_file"].is_string()) {
        rules_file_ = j["rules_file"].get<std::string>();
      }
      if (j.contains("session_override_file") && j["session_override_file"].is_string()) {
        session_override_file_ = j["session_override_file"].get<std::string>();
      }
      if (j.contains("bridge_settings_file") && j["bridge_settings_file"].is_string()) {
        bridge_settings_file_ = j["bridge_settings_file"].get<std::string>();
      }
      if (j.contains("default_preset") && j["default_preset"].is_string()) {
        default_preset_ = j["default_preset"].get<std::string>();
      }
      if (j.contains("api_bind_host") && j["api_bind_host"].is_string()) {
        api_bind_host_ = j["api_bind_host"].get<std::string>();
      }
      if (j.contains("api_port") && j["api_port"].is_number_integer()) {
        api_port_ = j["api_port"].get<int>();
      }
      if (j.contains("allow_unprefixed_local") && j["allow_unprefixed_local"].is_boolean()) {
        allow_unprefixed_local_ = j["allow_unprefixed_local"].get<bool>();
      }
      if (j.contains("liveliness_enabled") && j["liveliness_enabled"].is_boolean()) {
        liveliness_enabled_ = j["liveliness_enabled"].get<bool>();
      }
      if (j.contains("session") && j["session"].is_object()) {
        session_cfg_ = j["session"].get<SessionConfig>();
      }
    } catch (const std::exception &e) {
      RCLCPP_WARN(this->get_logger(), "Failed to parse runtime_config_file '%s': %s",
                  runtime_config_file_.c_str(), e.what());
    }
  }

  // Initialize bridge_settings_ from runtime defaults, then overlay persisted bridge_settings.json
  bridge_settings_.agent_id = (agent_id_ == default_agent_id_) ? "" : agent_id_;
  bridge_settings_.domain_prefix = domain_prefix_;
  bridge_settings_.default_preset = default_preset_;
  bridge_settings_.allow_unprefixed_local = allow_unprefixed_local_;
  bridge_settings_.liveliness_enabled = liveliness_enabled_;

  if (!bridge_settings_file_.empty() && std::filesystem::exists(bridge_settings_file_)) {
    try {
      std::ifstream ifs(bridge_settings_file_);
      nlohmann::json j;
      ifs >> j;
      bridge_settings_ = j.get<BridgeSettings>();
      agent_id_ = bridge_settings_.agent_id.empty() ? default_agent_id_ : bridge_settings_.agent_id;
      domain_prefix_ = bridge_settings_.domain_prefix.empty() ? kDefaultDomainPrefix : bridge_settings_.domain_prefix;
      default_preset_ = bridge_settings_.default_preset;
      allow_unprefixed_local_ = bridge_settings_.allow_unprefixed_local;
      liveliness_enabled_ = bridge_settings_.liveliness_enabled;
      RCLCPP_INFO(this->get_logger(), "Loaded UI bridge settings from %s", bridge_settings_file_.c_str());
    } catch (const std::exception &e) {
      RCLCPP_WARN(this->get_logger(), "Failed to load bridge_settings_file '%s': %s",
                  bridge_settings_file_.c_str(), e.what());
    }
  }

  // Apply persisted UI session overrides if present
  if (!session_override_file_.empty() && std::filesystem::exists(session_override_file_)) {
    try {
      std::ifstream ifs(session_override_file_);
      nlohmann::json j;
      ifs >> j;
      session_cfg_ = j.get<SessionConfig>();
      RCLCPP_INFO(this->get_logger(), "Loaded UI session override from %s",
                  session_override_file_.c_str());
    } catch (const std::exception &e) {
      RCLCPP_WARN(this->get_logger(), "Failed to load session_override_file '%s': %s",
                  session_override_file_.c_str(), e.what());
    }
  }
}

zenoh::Config ZenohBridgeNode::buildZenohConfig(const SessionConfig &sc,
                                                std::string &out_effective_json) const {
  if (sc.config_mode == "raw_json5") {
    const std::string raw = trimWhitespace(sc.raw_json5);
    if (raw.empty()) {
      auto cfg = zenoh::Config::create_default();
      out_effective_json = cfg.to_string();
      return cfg;
    }
    auto cfg = zenoh::Config::from_str(raw);
    out_effective_json = cfg.to_string();
    return cfg;
  }

  zenoh::Config cfg = zenoh::Config::create_default();

  // 1. Identity, Mode, Namespace & Metadata
  if (!trimWhitespace(sc.custom_zid).empty()) {
    strictInsertJson5(cfg, "id", nlohmann::json(trimWhitespace(sc.custom_zid)).dump());
  }
  if (!trimWhitespace(sc.zenoh_namespace).empty()) {
    strictInsertJson5(cfg, "namespace", nlohmann::json(trimSlashes(trimWhitespace(sc.zenoh_namespace))).dump());
  }
  if (!trimWhitespace(sc.metadata_json5).empty()) {
    strictInsertJson5(cfg, "metadata", trimWhitespace(sc.metadata_json5));
  }
  tryInsertJson5(cfg, "mode", nlohmann::json(sc.mode.empty() ? "peer" : sc.mode).dump(), get_logger());

  // 2. Connect & Listen Endpoints
  if (!sc.connect_endpoints.empty()) {
    strictInsertJson5(cfg, "connect/endpoints", jsonArrayString(sc.connect_endpoints));
  }
  if (sc.connect_timeout_ms > 0) {
    tryInsertJson5(cfg, "connect/timeout_ms", std::to_string(sc.connect_timeout_ms), get_logger());
  }
  tryInsertJson5(cfg, "connect/exit_on_failure", sc.connect_exit_on_failure ? "true" : "false", get_logger());

  if (!sc.listen_endpoints.empty()) {
    strictInsertJson5(cfg, "listen/endpoints", jsonArrayString(sc.listen_endpoints));
  }
  if (sc.listen_timeout_ms > 0) {
    tryInsertJson5(cfg, "listen/timeout_ms", std::to_string(sc.listen_timeout_ms), get_logger());
  }
  tryInsertJson5(cfg, "listen/exit_on_failure", sc.listen_exit_on_failure ? "true" : "false", get_logger());

  if (sc.queries_default_timeout_ms > 0) {
    tryInsertJson5(cfg, "queries_default_timeout", std::to_string(sc.queries_default_timeout_ms), get_logger());
  }

  // 3. Scouting (Multicast & Gossip)
  if (sc.scouting_timeout_ms > 0) {
    tryInsertJson5(cfg, "scouting/timeout", std::to_string(sc.scouting_timeout_ms), get_logger());
  }
  if (sc.scouting_delay_ms >= 0) {
    tryInsertJson5(cfg, "scouting/delay", std::to_string(sc.scouting_delay_ms), get_logger());
  }
  tryInsertJson5(cfg, "scouting/multicast/enabled", sc.multicast_enabled ? "true" : "false", get_logger());
  if (!trimWhitespace(sc.multicast_address).empty()) {
    tryInsertJson5(cfg, "scouting/multicast/address",
                   nlohmann::json(trimWhitespace(sc.multicast_address)).dump(), get_logger());
  }
  if (!trimWhitespace(sc.multicast_interface).empty()) {
    tryInsertJson5(cfg, "scouting/multicast/interface",
                   nlohmann::json(trimWhitespace(sc.multicast_interface)).dump(), get_logger());
  }
  if (sc.multicast_ttl > 0) {
    tryInsertJson5(cfg, "scouting/multicast/ttl", std::to_string(sc.multicast_ttl), get_logger());
  }
  tryInsertJson5(cfg, "scouting/multicast/listen", sc.multicast_listen ? "true" : "false", get_logger());
  tryInsertJson5(cfg, "scouting/gossip/enabled", sc.gossip_enabled ? "true" : "false", get_logger());
  tryInsertJson5(cfg, "scouting/gossip/multihop", sc.gossip_multihop ? "true" : "false", get_logger());

  // 4. Transport & Link Tuning
  tryInsertJson5(cfg, "transport/unicast/lowlatency", sc.unicast_lowlatency ? "true" : "false", get_logger());
  tryInsertJson5(cfg, "transport/unicast/qos/enabled", sc.unicast_qos_enabled ? "true" : "false", get_logger());
  tryInsertJson5(cfg, "transport/unicast/compression/enabled", sc.compression_enabled ? "true" : "false", get_logger());
  if (sc.unicast_max_sessions > 0) {
    tryInsertJson5(cfg, "transport/unicast/max_sessions", std::to_string(sc.unicast_max_sessions), get_logger());
  }
  if (sc.unicast_max_links > 0) {
    tryInsertJson5(cfg, "transport/unicast/max_links", std::to_string(sc.unicast_max_links), get_logger());
  }
  if (sc.unicast_open_timeout > 0) {
    tryInsertJson5(cfg, "transport/unicast/open_timeout", std::to_string(sc.unicast_open_timeout), get_logger());
  }
  if (sc.unicast_accept_timeout > 0) {
    tryInsertJson5(cfg, "transport/unicast/accept_timeout", std::to_string(sc.unicast_accept_timeout), get_logger());
  }
  tryInsertJson5(cfg, "transport/multicast/qos/enabled", sc.multicast_qos_enabled ? "true" : "false", get_logger());
  tryInsertJson5(cfg, "transport/multicast/compression/enabled",
                 sc.multicast_compression_enabled ? "true" : "false", get_logger());
  if (sc.multicast_join_interval > 0) {
    tryInsertJson5(cfg, "transport/multicast/join_interval",
                   std::to_string(sc.multicast_join_interval), get_logger());
  }
  if (sc.lease_ms > 0) {
    tryInsertJson5(cfg, "transport/link/tx/lease", std::to_string(sc.lease_ms), get_logger());
  }
  if (sc.keep_alive > 0) {
    tryInsertJson5(cfg, "transport/link/tx/keep_alive", std::to_string(sc.keep_alive), get_logger());
  }
  if (sc.tx_batch_size > 0) {
    tryInsertJson5(cfg, "transport/link/tx/batch_size", std::to_string(sc.tx_batch_size), get_logger());
  }
  if (sc.tx_threads > 0) {
    tryInsertJson5(cfg, "transport/link/tx/threads", std::to_string(sc.tx_threads), get_logger());
  }
  tryInsertJson5(cfg, "transport/link/tx/queue/batching/enabled",
                 sc.batching_enabled ? "true" : "false", get_logger());
  if (sc.batching_time_limit_ms >= 0) {
    tryInsertJson5(cfg, "transport/link/tx/queue/batching/time_limit",
                   std::to_string(sc.batching_time_limit_ms), get_logger());
  }
  if (sc.rx_buffer_size > 0) {
    tryInsertJson5(cfg, "transport/link/rx/buffer_size", std::to_string(sc.rx_buffer_size), get_logger());
  }
  if (sc.rx_max_message_size > 0) {
    tryInsertJson5(cfg, "transport/link/rx/max_message_size",
                   std::to_string(sc.rx_max_message_size), get_logger());
  }
  if (sc.tcp_so_sndbuf > 0) {
    tryInsertJson5(cfg, "transport/link/tcp/so_sndbuf", std::to_string(sc.tcp_so_sndbuf), get_logger());
  }
  if (sc.tcp_so_rcvbuf > 0) {
    tryInsertJson5(cfg, "transport/link/tcp/so_rcvbuf", std::to_string(sc.tcp_so_rcvbuf), get_logger());
  }
  if (!trimWhitespace(sc.tx_queue_json5).empty()) {
    strictInsertJson5(cfg, "transport/link/tx/queue", trimWhitespace(sc.tx_queue_json5));
  }

  // 5. Shared Memory (SHM)
  tryInsertJson5(cfg, "transport/shared_memory/enabled", sc.shared_memory_enabled ? "true" : "false", get_logger());
  if (!sc.shm_mode.empty()) {
    tryInsertJson5(cfg, "transport/shared_memory/mode", nlohmann::json(sc.shm_mode).dump(), get_logger());
  }
  tryInsertJson5(cfg, "transport/shared_memory/transport_optimization/enabled",
                 sc.shm_opt_enabled ? "true" : "false", get_logger());
  if (sc.shm_pool_size > 0) {
    tryInsertJson5(cfg, "transport/shared_memory/transport_optimization/pool_size",
                   std::to_string(sc.shm_pool_size), get_logger());
  }
  if (sc.shm_message_threshold > 0) {
    tryInsertJson5(cfg, "transport/shared_memory/transport_optimization/message_size_threshold",
                   std::to_string(sc.shm_message_threshold), get_logger());
  }

  // 6. Timestamping
  tryInsertJson5(cfg, "timestamping/enabled", sc.timestamping_enabled ? "true" : "false", get_logger());
  tryInsertJson5(cfg, "timestamping/drop_future_timestamp",
                 sc.drop_future_timestamp ? "true" : "false", get_logger());

  // 7. Security: TLS / mTLS / Auth / ACL
  if (!trimWhitespace(sc.tls_root_ca).empty()) {
    strictInsertJson5(cfg, "transport/link/tls/root_ca_certificate",
                      nlohmann::json(trimWhitespace(sc.tls_root_ca)).dump());
  }
  if (!trimWhitespace(sc.tls_listen_key).empty()) {
    strictInsertJson5(cfg, "transport/link/tls/listen_private_key",
                      nlohmann::json(trimWhitespace(sc.tls_listen_key)).dump());
  }
  if (!trimWhitespace(sc.tls_listen_cert).empty()) {
    strictInsertJson5(cfg, "transport/link/tls/listen_certificate",
                      nlohmann::json(trimWhitespace(sc.tls_listen_cert)).dump());
  }
  if (sc.tls_enable_mtls) {
    strictInsertJson5(cfg, "transport/link/tls/enable_mtls", "true");
  }
  if (!trimWhitespace(sc.tls_connect_key).empty()) {
    strictInsertJson5(cfg, "transport/link/tls/connect_private_key",
                      nlohmann::json(trimWhitespace(sc.tls_connect_key)).dump());
  }
  if (!trimWhitespace(sc.tls_connect_cert).empty()) {
    strictInsertJson5(cfg, "transport/link/tls/connect_certificate",
                      nlohmann::json(trimWhitespace(sc.tls_connect_cert)).dump());
  }
  if (!sc.tls_verify_name) {
    tryInsertJson5(cfg, "transport/link/tls/verify_name_on_connect", "false", get_logger());
  }

  if (!trimWhitespace(sc.auth_user).empty()) {
    strictInsertJson5(cfg, "transport/auth/usrpwd/user", nlohmann::json(trimWhitespace(sc.auth_user)).dump());
  }
  if (!sc.auth_password.empty()) {
    strictInsertJson5(cfg, "transport/auth/usrpwd/password", nlohmann::json(sc.auth_password).dump());
  }
  if (!trimWhitespace(sc.auth_dictionary_file).empty()) {
    strictInsertJson5(cfg, "transport/auth/usrpwd/dictionary_file",
                      nlohmann::json(trimWhitespace(sc.auth_dictionary_file)).dump());
  }
  if (!trimWhitespace(sc.auth_pubkey_file).empty()) {
    strictInsertJson5(cfg, "transport/auth/pubkey/public_key_file",
                      nlohmann::json(trimWhitespace(sc.auth_pubkey_file)).dump());
  }
  if (!trimWhitespace(sc.auth_privkey_file).empty()) {
    strictInsertJson5(cfg, "transport/auth/pubkey/private_key_file",
                      nlohmann::json(trimWhitespace(sc.auth_privkey_file)).dump());
  }
  if (!trimWhitespace(sc.auth_known_keys_file).empty()) {
    strictInsertJson5(cfg, "transport/auth/pubkey/known_keys_file",
                      nlohmann::json(trimWhitespace(sc.auth_known_keys_file)).dump());
  }

  if (!trimWhitespace(sc.acl_json5).empty()) {
    strictInsertJson5(cfg, "access_control", trimWhitespace(sc.acl_json5));
  } else {
    tryInsertJson5(cfg, "access_control/enabled", sc.acl_enabled ? "true" : "false", get_logger());
    if (!sc.acl_default_permission.empty()) {
      tryInsertJson5(cfg, "access_control/default_permission",
                     nlohmann::json(sc.acl_default_permission).dump(), get_logger());
    }
  }

  // 8. Routing, AdminSpace, Kernel Flow Control & Plugins
  tryInsertJson5(cfg, "routing/router/peers_failover_brokering",
                 sc.peers_failover_brokering ? "true" : "false", get_logger());
  if (!sc.peer_routing_mode.empty()) {
    tryInsertJson5(cfg, "routing/peer/mode", nlohmann::json(sc.peer_routing_mode).dump(), get_logger());
  }
  tryInsertJson5(cfg, "adminspace/enabled", sc.adminspace_enabled ? "true" : "false", get_logger());
  tryInsertJson5(cfg, "adminspace/permissions/read", sc.adminspace_read ? "true" : "false", get_logger());
  tryInsertJson5(cfg, "adminspace/permissions/write", sc.adminspace_write ? "true" : "false", get_logger());

  if (!trimWhitespace(sc.downsampling_json5).empty()) {
    strictInsertJson5(cfg, "downsampling", trimWhitespace(sc.downsampling_json5));
  }
  if (!trimWhitespace(sc.low_pass_filter_json5).empty()) {
    strictInsertJson5(cfg, "low_pass_filter", trimWhitespace(sc.low_pass_filter_json5));
  }
  if (!trimWhitespace(sc.qos_publication_json5).empty()) {
    strictInsertJson5(cfg, "qos/publication", trimWhitespace(sc.qos_publication_json5));
  }
  if (!trimWhitespace(sc.qos_network_json5).empty()) {
    strictInsertJson5(cfg, "qos/network", trimWhitespace(sc.qos_network_json5));
  }
  if (!trimWhitespace(sc.aggregation_json5).empty()) {
    strictInsertJson5(cfg, "aggregation", trimWhitespace(sc.aggregation_json5));
  }
  tryInsertJson5(cfg, "plugins_loading/enabled", sc.plugins_loading_enabled ? "true" : "false", get_logger());
  if (!trimWhitespace(sc.plugins_json5).empty()) {
    strictInsertJson5(cfg, "plugins", trimWhitespace(sc.plugins_json5));
  }

  // 9. Arbitrary True JSON5 Override (parsed natively via Zenoh's own JSON5 parser!)
  const std::string custom = trimWhitespace(sc.custom_json5);
  if (!custom.empty()) {
    zenoh::Config def_cfg = zenoh::Config::create_default();
    zenoh::Config ovr_cfg = zenoh::Config::from_str(custom);
    const auto def_json = nlohmann::json::parse(def_cfg.to_string());
    const auto ovr_json = nlohmann::json::parse(ovr_cfg.to_string());
    mergeJsonDiffIntoZenohConfig(cfg, def_json, ovr_json, "");
  }

  out_effective_json = cfg.to_string();
  return cfg;
}

void ZenohBridgeNode::initZenohSession() {
  std::lock_guard<std::mutex> lk(session_mutex_);

  std::string effective_json;
  zenoh::Config cfg = buildZenohConfig(session_cfg_, effective_json);
  effective_zenoh_config_json_ = effective_json;

  // Sync effective mode back for display if raw_json5 changed it
  try {
    const auto parsed = nlohmann::json::parse(effective_json);
    if (parsed.contains("mode") && parsed["mode"].is_string()) {
      session_cfg_.mode = parsed["mode"].get<std::string>();
    }
  } catch (...) {
  }

  session_.emplace(zenoh::Session::open(std::move(cfg)));

  auto status_opts = zenoh::Session::PublisherOptions::create_default();
  status_opts.priority = Z_PRIORITY_DATA_LOW;
  status_opts.congestion_control = Z_CONGESTION_CONTROL_DROP;
  status_pub_.emplace(session_->declare_publisher(
      zenoh::KeyExpr(makeStatusKey(domain_prefix_, agent_id_)), std::move(status_opts)));

  ctrl_queryable_.emplace(session_->declare_queryable(
      zenoh::KeyExpr(makeControlKey(domain_prefix_, agent_id_)),
      [this](const zenoh::Query &query) { handleControlQuery(query); },
      zenoh::closures::none));

  swarm_status_sub_.emplace(session_->declare_subscriber(
      zenoh::KeyExpr(makeStatusWildcard(domain_prefix_)),
      [this](const zenoh::Sample &sample) { handlePeerStatusSample(sample); },
      zenoh::closures::none));

  // Official Zenoh Liveliness Token & Subscriber
  if (liveliness_enabled_) {
    try {
      const std::string live_key = makeLivelinessKey(domain_prefix_, agent_id_);
      own_liveliness_token_.emplace(session_->liveliness_declare_token(zenoh::KeyExpr(live_key)));

      auto live_sub_opts = zenoh::Session::LivelinessSubscriberOptions::create_default();
      live_sub_opts.history = true;
      liveliness_sub_.emplace(session_->liveliness_declare_subscriber(
          zenoh::KeyExpr(makeLivelinessWildcard(domain_prefix_)),
          [this](const zenoh::Sample &sample) {
            const std::string key(sample.get_keyexpr().as_string_view());
            const bool alive = (sample.get_kind() == Z_SAMPLE_KIND_PUT);
            {
              std::lock_guard<std::mutex> llk(liveliness_mutex_);
              if (alive) {
                observed_liveliness_keys_.insert(key);
              } else {
                observed_liveliness_keys_.erase(key);
              }
            }
            addLog("INFO", std::string("Liveliness ") + (alive ? "ONLINE: " : "OFFLINE: ") + key);
          },
          zenoh::closures::none, std::move(live_sub_opts)));
    } catch (const std::exception &e) {
      RCLCPP_WARN(get_logger(), "Liveliness setup skipped: %s", e.what());
    }
  }

  // Official Zenoh 1.8 Transport & Link Events Listeners
#if defined(Z_FEATURE_UNSTABLE_API)
  try {
    auto t_opts = zenoh::Session::TransportEventsListenerOptions::create_default();
    t_opts.history = false;
    transport_events_listener_.emplace(session_->declare_transport_events_listener(
        [this](const zenoh::TransportEvent &ev) {
          const bool opened = (ev.get_kind() == Z_SAMPLE_KIND_PUT);
          const auto &tr = ev.get_transport();
          const std::string zid = tr.get_zid().to_string();
          const std::string what(zenoh::whatami_as_str(tr.get_whatami()));
          addLog("INFO", std::string("Transport ") + (opened ? "OPENED: " : "CLOSED: ") +
                             what + " (" + zid.substr(0, 12) + "...)");
        },
        zenoh::closures::none, std::move(t_opts)));

    auto l_opts = zenoh::Session::LinkEventsListenerOptions::create_default();
    l_opts.history = false;
    link_events_listener_.emplace(session_->declare_link_events_listener(
        [this](const zenoh::LinkEvent &ev) {
          const bool added = (ev.get_kind() == Z_SAMPLE_KIND_PUT);
          const auto &lnk = ev.get_link();
          addLog("INFO", std::string("Link ") + (added ? "UP: " : "DOWN: ") +
                             lnk.get_src() + " -> " + lnk.get_dst() +
                             " (mtu=" + std::to_string(lnk.get_mtu()) + ")");
        },
        zenoh::closures::none, std::move(l_opts)));
  } catch (const std::exception &e) {
    RCLCPP_WARN(get_logger(), "Connectivity event listeners skipped: %s", e.what());
  }
#endif
}

void ZenohBridgeNode::teardownZenohSession() {
  {
    std::lock_guard<std::mutex> slk(sniff_mutex_);
    sniff_active_.store(false, std::memory_order_release);
    sniff_sub_.reset();
  }
  std::map<std::string, zenoh::LivelinessToken> old_tokens;
  {
    std::lock_guard<std::mutex> llk(liveliness_mutex_);
    old_tokens.swap(custom_liveliness_tokens_);
    observed_liveliness_keys_.clear();
  }
  old_tokens.clear();

  std::vector<std::shared_ptr<ActiveEgress>> old_egress;
  std::vector<std::shared_ptr<ActiveIngress>> old_ingress;
  {
    std::lock_guard<std::mutex> lk(state_mutex_);
    for (auto &e : egress_list_) e->active.store(false, std::memory_order_relaxed);
    for (auto &i : ingress_list_) i->active.store(false, std::memory_order_relaxed);
    old_egress.swap(egress_list_);
    old_ingress.swap(ingress_list_);
    ingress_owned_topics_.clear();
  }
  old_egress.clear();
  old_ingress.clear();

  std::lock_guard<std::mutex> lk(session_mutex_);
#if defined(Z_FEATURE_UNSTABLE_API)
  link_events_listener_.reset();
  transport_events_listener_.reset();
#endif
  liveliness_sub_.reset();
  own_liveliness_token_.reset();
  swarm_status_sub_.reset();
  ctrl_queryable_.reset();
  status_pub_.reset();
  session_.reset();
}

bool ZenohBridgeNode::reconnectSession(const SessionConfig &new_cfg, bool persist, std::string &out_msg) {
  // Dry-run validate BEFORE tearing down active session!
  try {
    std::string preview;
    auto test_cfg = buildZenohConfig(new_cfg, preview);
    (void)test_cfg;
  } catch (const std::exception &e) {
    out_msg = std::string("Invalid Zenoh configuration: ") + e.what();
    addLog("ERROR", out_msg);
    return false;
  }

  RuleSet saved_rules = getCurrentRules();
  SessionConfig backup_cfg = session_cfg_;

  teardownZenohSession();
  session_cfg_ = new_cfg;
  try {
    initZenohSession();
  } catch (const std::exception &e) {
    out_msg = std::string("Failed to open Zenoh session with new config: ") + e.what();
    RCLCPP_ERROR(get_logger(), "%s — reverting to previous config", out_msg.c_str());
    addLog("ERROR", out_msg);
    session_cfg_ = backup_cfg;
    try {
      initZenohSession();
      std::string dummy;
      applyRules(saved_rules, false, dummy);
    } catch (...) {
    }
    return false;
  }

  std::string apply_msg;
  applyRules(saved_rules, false, apply_msg);

  if (persist && !session_override_file_.empty()) {
    try {
      atomicWriteFile(session_override_file_, nlohmann::json(session_cfg_).dump(2) + "\n");
    } catch (const std::exception &e) {
      out_msg = std::string("Session reconnected, but failed to persist override: ") + e.what();
      return true;
    }
  }

  out_msg = "Zenoh session reconnected (mode=" + session_cfg_.mode +
            ", config_mode=" + session_cfg_.config_mode + ")";
  addLog("INFO", out_msg);
  return true;
}

bool ZenohBridgeNode::updateBridgeSettings(const BridgeSettings &new_settings,
                                           bool persist,
                                           std::string &out_msg) {
  BridgeSettings clean = new_settings;
  clean.agent_id = trimSlashes(trimWhitespace(clean.agent_id));
  clean.domain_prefix = trimSlashes(trimWhitespace(clean.domain_prefix));
  if (clean.domain_prefix.empty()) clean.domain_prefix = kDefaultDomainPrefix;
  if (clean.default_preset.empty()) clean.default_preset = "empty";

  const std::string next_agent_id = clean.agent_id.empty() ? default_agent_id_ : clean.agent_id;
  const bool need_reconnect = (next_agent_id != agent_id_) ||
                              (clean.domain_prefix != domain_prefix_) ||
                              (clean.liveliness_enabled != liveliness_enabled_);

  const std::string old_agent_id = agent_id_;
  bridge_settings_ = clean;
  agent_id_ = next_agent_id;
  domain_prefix_ = clean.domain_prefix;
  default_preset_ = clean.default_preset;
  allow_unprefixed_local_ = clean.allow_unprefixed_local;
  liveliness_enabled_ = clean.liveliness_enabled;

  if (old_agent_id != agent_id_) {
    std::lock_guard<std::mutex> plk(peers_mutex_);
    remote_agents_.erase(old_agent_id);
    remote_last_seen_ms_.erase(old_agent_id);
  }

  if (persist && !bridge_settings_file_.empty()) {
    try {
      atomicWriteFile(bridge_settings_file_, nlohmann::json(bridge_settings_).dump(2) + "\n");
    } catch (const std::exception &e) {
      out_msg = std::string("Failed to persist bridge_settings.json: ") + e.what();
      return false;
    }
  }

  if (need_reconnect) {
    std::string rc_msg;
    if (!reconnectSession(session_cfg_, false, rc_msg)) {
      out_msg = "Settings saved, but reconnect failed: " + rc_msg;
      return false;
    }
  } else {
    attachEgressRosSubscriptions();
  }

  out_msg = "Bridge settings updated (agent_id=" + agent_id_ +
            ", prefix=" + domain_prefix_ + ", liveliness=" +
            (liveliness_enabled_ ? "on" : "off") + ")";
  addLog("INFO", out_msg);
  return true;
}

void ZenohBridgeNode::loadInitialRules() {
  RuleSet rules;
  if (!rules_file_.empty() && std::filesystem::exists(rules_file_)) {
    try {
      rules = loadRulesFromYamlFile(rules_file_);
    } catch (const std::exception &e) {
      RCLCPP_WARN(get_logger(), "Failed to load rules_file '%s': %s. Using preset '%s'.",
                  rules_file_.c_str(), e.what(), default_preset_.c_str());
      rules = makePresetRules(default_preset_);
    }
  } else {
    rules = makePresetRules(default_preset_);
  }
  std::string msg;
  applyRules(rules, false, msg);
}

RuleSet ZenohBridgeNode::getCurrentRules() const {
  std::lock_guard<std::mutex> lk(state_mutex_);
  RuleSet rs;
  rs.egress.reserve(egress_list_.size());
  for (const auto &e : egress_list_) {
    EgressRule r = e->rule;
    r.resolved_type = e->resolved_type;
    r.bound_topic = e->local_topic;
    r.effective_key = e->zenoh_key;
    r.last_error = e->last_error;
    r.actual_hz = e->last_hz;
    r.kbps = e->last_kbps;
    r.tx_count = e->tx_count.load(std::memory_order_relaxed);
    r.dropped_count = e->dropped_count.load(std::memory_order_relaxed);
    r.has_subscribers = e->has_subscribers.load(std::memory_order_relaxed);
    rs.egress.push_back(std::move(r));
  }
  rs.ingress.reserve(ingress_list_.size());
  for (const auto &i : ingress_list_) {
    IngressRule r = i->rule;
    r.actual_hz = i->last_hz;
    r.kbps = i->last_kbps;
    r.rx_count = i->rx_count.load(std::memory_order_relaxed);
    r.dropped_count = i->dropped_count.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> elk(i->endpoints_mutex);
      for (const auto &kv : i->endpoints) {
        r.active_endpoints.push_back(kv.first);
      }
      r.last_error = i->last_error;
    }
    rs.ingress.push_back(std::move(r));
  }
  return rs;
}

bool ZenohBridgeNode::applyRules(const RuleSet &new_rules, bool persist_to_file, std::string &out_msg) {
  std::lock_guard<std::mutex> rules_lk(rules_mutex_);
  std::vector<std::shared_ptr<ActiveEgress>> next_egress;
  std::vector<std::shared_ptr<ActiveIngress>> next_ingress;
  next_egress.reserve(new_rules.egress.size());
  next_ingress.reserve(new_rules.ingress.size());

  {
    std::lock_guard<std::mutex> slk(session_mutex_);
    if (!session_.has_value()) {
      out_msg = "Zenoh session is not initialized";
      return false;
    }

    try {
      std::set<std::string> seen_egress;
      for (const auto &raw_rule : new_rules.egress) {
        const std::string rel = normalizeRelativeTopic(raw_rule.topic, agent_id_);
        if (rel.empty() || !seen_egress.insert(rel).second) {
          continue;
        }
        auto eg = std::make_shared<ActiveEgress>();
        eg->rule = raw_rule;
        eg->rule.topic = rel;
        eg->rule.priority = normalizePriorityName(raw_rule.priority);
        eg->rule.locality = normalizeLocalityName(raw_rule.locality);
        eg->rule.history_depth = std::max(1, std::min(1000, raw_rule.history_depth));
        eg->rel_topic = rel;
        eg->zenoh_key = makeDataKey(domain_prefix_, agent_id_, rel, raw_rule.zenoh_key);

        if (eg->rule.enabled) {
          auto pub_opts = zenoh::Session::PublisherOptions::create_default();
          pub_opts.priority = parsePriority(eg->rule.priority);
          pub_opts.congestion_control = parseCongestion(eg->rule.congestion);
          pub_opts.is_express = eg->rule.express;
          pub_opts.allowed_destination = parseLocality(eg->rule.locality);
#if defined(Z_FEATURE_UNSTABLE_API)
          pub_opts.reliability = (eg->rule.reliability == "best_effort")
                                     ? Z_RELIABILITY_BEST_EFFORT
                                     : Z_RELIABILITY_RELIABLE;
#endif
          eg->z_pub.emplace(session_->declare_publisher(
              zenoh::KeyExpr(eg->zenoh_key), std::move(pub_opts)));

          try {
            eg->has_subscribers.store(eg->z_pub->get_matching_status().matching,
                                      std::memory_order_relaxed);
            std::weak_ptr<ActiveEgress> weak_eg = eg;
            eg->matching_listener.emplace(eg->z_pub->declare_matching_listener(
                [weak_eg](const zenoh::MatchingStatus &status) {
                  if (auto locked = weak_eg.lock()) {
                    locked->has_subscribers.store(status.matching, std::memory_order_relaxed);
                  }
                },
                zenoh::closures::none));
          } catch (const std::exception &) {
            eg->has_subscribers.store(true, std::memory_order_relaxed);
          }
        }
        next_egress.push_back(std::move(eg));
      }

      for (const auto &raw_rule : new_rules.ingress) {
        const std::string norm_pat = trimSlashes(raw_rule.pattern);
        if (norm_pat.empty()) {
          continue;
        }
        auto rule = raw_rule;
        rule.pattern = norm_pat;
        rule.locality = normalizeLocalityName(raw_rule.locality);
        rule.history_depth = std::max(1, std::min(1000, raw_rule.history_depth));
        next_ingress.push_back(makeIngress(rule, false));
      }
    } catch (const std::exception &e) {
      out_msg = std::string("Failed to apply Zenoh rules: ") + e.what();
      RCLCPP_ERROR(get_logger(), "%s", out_msg.c_str());
      return false;
    }
  }

  const size_t applied_egress = next_egress.size();
  const size_t applied_ingress = next_ingress.size();

  std::vector<std::shared_ptr<ActiveEgress>> old_egress;
  std::vector<std::shared_ptr<ActiveIngress>> old_ingress;
  {
    std::lock_guard<std::mutex> lk(state_mutex_);
    for (auto &e : egress_list_) e->active.store(false, std::memory_order_relaxed);
    for (auto &i : ingress_list_) i->active.store(false, std::memory_order_relaxed);
    old_egress.swap(egress_list_);
    old_ingress.swap(ingress_list_);
    egress_list_ = std::move(next_egress);
    ingress_list_ = std::move(next_ingress);
    ingress_owned_topics_.clear();
    for (auto &i : ingress_list_) {
      i->active.store(true, std::memory_order_release);
    }
  }
  old_egress.clear();
  old_ingress.clear();

  attachEgressRosSubscriptions();

  bool saved = false;
  if (persist_to_file) {
    if (!rules_file_.empty()) {
      try {
        saveRulesToYamlFile(rules_file_, getCurrentRules());
        saved = true;
      } catch (const std::exception &e) {
        out_msg = std::string("Applied in memory, but save failed: ") + e.what();
        RCLCPP_WARN(get_logger(), "%s", out_msg.c_str());
        return false;
      }
    } else {
      out_msg = "Applied in memory (no rules_file configured)";
      return true;
    }
  }

  out_msg = "Applied " + std::to_string(applied_egress) + " egress and " +
            std::to_string(applied_ingress) + " ingress rules" +
            (saved ? " (saved)" : "");
  RCLCPP_INFO(get_logger(), "[%s] %s", agent_id_.c_str(), out_msg.c_str());
  addLog("INFO", out_msg);
  return true;
}

std::shared_ptr<ZenohBridgeNode::ActiveIngress> ZenohBridgeNode::makeIngress(
    const IngressRule &rule, bool initially_active) {
  auto ig = std::make_shared<ActiveIngress>();
  ig->active.store(initially_active, std::memory_order_relaxed);
  ig->rule = rule;
  ig->zenoh_pattern = makeDataSubKey(domain_prefix_, rule.pattern);
  if (rule.enabled) {
    auto sub_opts = zenoh::Session::SubscriberOptions::create_default();
    sub_opts.allowed_origin = parseLocality(rule.locality);
    std::weak_ptr<ActiveIngress> weak_ig = ig;
    ig->z_sub.emplace(session_->declare_subscriber(
        zenoh::KeyExpr(ig->zenoh_pattern),
        [this, weak_ig](const zenoh::Sample &sample) {
          if (auto locked = weak_ig.lock()) {
            handleZenohSample(locked, sample);
          }
        },
        zenoh::closures::none, std::move(sub_opts)));
  }
  return ig;
}

std::string ZenohBridgeNode::localIngressTopic(const IngressRule &rule,
                                               const std::string &source,
                                               const std::string &topic) const {
  if (!rule.remap_prefix.empty()) {
    const std::string pref = trimSlashes(rule.remap_prefix);
    return "/" + pref + "/" + trimSlashes(topic);
  }
  return "/" + trimSlashes(source) + "/" + trimSlashes(topic);
}

void ZenohBridgeNode::attachEgressRosSubscriptions() {
  std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);
  std::set<std::string> owned_ingress;
  std::vector<std::shared_ptr<ActiveEgress>> egress;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    owned_ingress = ingress_owned_topics_;
    egress = egress_list_;
  }

  std::map<std::string, std::vector<rclcpp::TopicEndpointInfo>> publishers;
  std::vector<LocalTopicInfo> discovered;
  for (const auto &entry : get_topic_names_and_types()) {
    if (isInternalSystemTopic(entry.first) || owned_ingress.count(entry.first)) continue;
    auto infos = get_publishers_info_by_topic(entry.first);
    auto sub_infos = get_subscriptions_info_by_topic(entry.first);
    if (infos.empty()) continue;

    std::map<std::string, std::vector<rclcpp::TopicEndpointInfo>> by_type;
    for (const auto &info : infos) {
      by_type[info.topic_type()].push_back(info);
    }
    for (const auto &kv : by_type) {
      LocalTopicInfo lt;
      lt.name = entry.first;
      lt.type = kv.first;
      lt.pub_count = static_cast<int>(kv.second.size());
      lt.sub_count = static_cast<int>(sub_infos.size());
      lt.reliability = reliabilityToStr(kv.second.front().qos_profile().reliability());
      lt.durability = durabilityToStr(kv.second.front().qos_profile().durability());
      discovered.push_back(std::move(lt));
    }
    publishers.emplace(entry.first, std::move(infos));
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    cached_local_topics_.swap(discovered);
  }

  for (const auto &eg : egress) {
    if (!eg->rule.enabled || !eg->active.load()) continue;
    std::string topic, type;
    const std::string bare = "/" + eg->rel_topic;
    const std::string prefixed = "/" + agent_id_ + bare;
    const auto select = [&](const std::string &name) {
      const auto it = publishers.find(name);
      if (it == publishers.end()) return false;
      for (const auto &info : it->second) {
        if (!eg->rule.type.empty() && eg->rule.type != info.topic_type()) continue;
        if (type.empty() || info.topic_type() < type) type = info.topic_type();
      }
      if (type.empty()) return false;
      topic = name;
      return true;
    };
    if (!select(prefixed) && !(allow_unprefixed_local_ && select(bare))) {
      if (eg->ros_sub) {
        ++eg->binding_generation;
        eg->ros_sub.reset();
        std::lock_guard<std::mutex> lock(state_mutex_);
        eg->local_topic.clear();
        eg->resolved_type.clear();
      }
      continue;
    }

    const auto qos = resolveSubscriptionQos(publishers.at(topic), type, eg->rule.qos, eg->rule.history_depth);
    if (eg->ros_sub && eg->local_topic == topic && eg->resolved_type == type &&
        eg->ros_sub->get_actual_qos().reliability() == qos.reliability() &&
        eg->ros_sub->get_actual_qos().durability() == qos.durability()) {
      continue;
    }
    const std::string reliability = (qos.reliability() == rclcpp::ReliabilityPolicy::BestEffort)
                                        ? "best_effort"
                                        : "reliable";
    const std::string durability = (qos.durability() == rclcpp::DurabilityPolicy::TransientLocal)
                                       ? "transient_local"
                                       : "volatile";
    const auto metadata =
        nlohmann::json{{"t", type}, {"s", agent_id_}, {"q", reliability}, {"d", durability}}.dump();
    const uint64_t generation = eg->binding_generation.load() + 1;
    try {
      std::weak_ptr<ActiveEgress> weak = eg;
      auto sub = create_generic_subscription(
          topic, type, qos,
          [this, weak, generation, metadata](std::shared_ptr<const rclcpp::SerializedMessage> msg) {
            if (auto current = weak.lock(); current && current->binding_generation.load() == generation) {
              handleRosMessage(current, msg, metadata);
            }
          });
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!eg->active.load() || ingress_owned_topics_.count(topic)) continue;
      eg->ros_sub.swap(sub);
      eg->local_topic = topic;
      eg->resolved_type = type;
      eg->last_error.clear();
      eg->binding_generation.store(generation);
      RCLCPP_INFO(get_logger(), "[%s] Egress bound: %s (%s, qos=%s/%s) -> %s",
                  agent_id_.c_str(), topic.c_str(), type.c_str(),
                  reliability.c_str(), durability.c_str(), eg->zenoh_key.c_str());
      addLog("INFO", "Egress bound: " + topic + " (" + type + ", qos=" + reliability + "/" +
                         durability + ") -> " + eg->zenoh_key);
    } catch (const std::exception &e) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        eg->last_error = e.what();
      }
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                           "Could not subscribe to %s (%s): %s", topic.c_str(), type.c_str(), e.what());
      addLog("ERROR", "Egress subscribe failed for " + topic + " (" + type + "): " + e.what());
    }
  }
}

void ZenohBridgeNode::onDiscoveryTimer() {
  attachEgressRosSubscriptions();
}

void ZenohBridgeNode::handleRosMessage(
    const std::shared_ptr<ActiveEgress> &eg,
    const std::shared_ptr<const rclcpp::SerializedMessage> &msg,
    const std::string &attachment) {
  if (!eg->active.load(std::memory_order_relaxed) || !eg->z_pub.has_value()) {
    return;
  }
  if (eg->rule.on_demand && !eg->has_subscribers.load(std::memory_order_relaxed)) {
    eg->dropped_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!passRateLimit(eg->last_sent_ns, eg->rule.max_freq)) {
    eg->dropped_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const auto &rcl_msg = msg->get_rcl_serialized_message();
  if (rcl_msg.buffer == nullptr || rcl_msg.buffer_length == 0) {
    return;
  }
  if (!passBandwidthLimit(eg->bw_window_start_ns, eg->bw_window_bytes,
                          rcl_msg.buffer_length, eg->rule.max_kbps)) {
    eg->dropped_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  try {
    std::vector<uint8_t> payload_buf(rcl_msg.buffer, rcl_msg.buffer + rcl_msg.buffer_length);
    zenoh::Bytes payload(std::move(payload_buf));
    auto put_opts = zenoh::Publisher::PutOptions::create_default();
    put_opts.attachment = zenoh::Bytes(attachment);
    put_opts.encoding = zenoh::Encoding::Predefined::application_octet_stream();
    eg->z_pub->put(std::move(payload), std::move(put_opts));

    eg->tx_count.fetch_add(1, std::memory_order_relaxed);
    eg->window_msgs.fetch_add(1, std::memory_order_relaxed);
    eg->window_bytes.fetch_add(rcl_msg.buffer_length, std::memory_order_relaxed);
  } catch (const std::exception &e) {
    eg->dropped_count.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      eg->last_error = e.what();
    }
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                         "[%s] Zenoh put failed on %s: %s",
                         agent_id_.c_str(), eg->zenoh_key.c_str(), e.what());
    addLog("WARN", "Zenoh put failed on " + eg->zenoh_key + ": " + e.what());
  }
}

void ZenohBridgeNode::handleZenohSample(const std::shared_ptr<ActiveIngress> &ig,
                                        const zenoh::Sample &sample) {
  if (!ig->active.load(std::memory_order_acquire)) {
    return;
  }

  const std::string full_key(sample.get_keyexpr().as_string_view());
  std::string src_agent;
  std::string rel_topic;
  if (!parseDataKey(domain_prefix_, full_key, src_agent, rel_topic)) {
    return;
  }

  std::string msg_type;
  std::string att_qos;
  std::string att_src;
  if (auto att = sample.get_attachment()) {
    try {
      const auto j = nlohmann::json::parse(att->get().as_string());
      msg_type = j.value("t", "");
      att_qos = j.value("q", "");
      att_src = j.value("s", "");
    } catch (const std::exception &) {
      return;
    }
  }
  if (!att_src.empty()) {
    src_agent = att_src;
  }
  if (ig->rule.exclude_self && src_agent == agent_id_) {
    return;
  }
  if (msg_type.empty()) {
    return;
  }

  const std::string local_topic = localIngressTopic(ig->rule, src_agent, rel_topic);

  std::shared_ptr<IngressEndpoint> ep;
  {
    std::lock_guard<std::mutex> lk(ig->endpoints_mutex);
    auto it = ig->endpoints.find(local_topic);
    if (it != ig->endpoints.end() && it->second->msg_type == msg_type) {
      ep = it->second;
    }
  }

  if (!ep) {
    try {
      {
        std::lock_guard<std::mutex> lk(state_mutex_);
        ingress_owned_topics_.insert(local_topic);
      }
      rclcpp::QoS pub_qos = buildPublisherQos(ig->rule.qos, att_qos, ig->rule.history_depth);
      auto pub = this->create_generic_publisher(local_topic, msg_type, pub_qos);

      auto new_ep = std::make_shared<IngressEndpoint>();
      new_ep->local_topic = local_topic;
      new_ep->msg_type = msg_type;
      new_ep->ros_pub = std::move(pub);

      std::lock_guard<std::mutex> lk(ig->endpoints_mutex);
      auto &slot = ig->endpoints[local_topic];
      if (!slot || slot->msg_type != msg_type) {
        slot = new_ep;
      }
      ig->last_error.clear();
      ep = slot;
      RCLCPP_INFO(get_logger(), "[%s] Ingress created: %s -> %s (%s)",
                  agent_id_.c_str(), full_key.c_str(), local_topic.c_str(), msg_type.c_str());
      addLog("INFO", "Ingress created: " + full_key + " -> " + local_topic + " (" + msg_type + ")");
    } catch (const std::exception &e) {
      {
        std::lock_guard<std::mutex> lk(ig->endpoints_mutex);
        ig->last_error = std::string(msg_type) + ": " + e.what();
      }
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                           "[%s] Failed to create ingress publisher %s (%s): %s",
                           agent_id_.c_str(), local_topic.c_str(), msg_type.c_str(), e.what());
      addLog("ERROR", "Ingress publisher failed for " + local_topic + " (" + msg_type + "): " + e.what());
      return;
    }
  }

  if (!passRateLimit(ep->last_rx_ns, ig->rule.max_freq)) {
    ig->dropped_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const std::vector<uint8_t> raw = sample.get_payload().as_vector();
  if (raw.empty()) {
    return;
  }

  try {
    rclcpp::SerializedMessage serialized_msg(raw.size());
    auto &rcl_msg = serialized_msg.get_rcl_serialized_message();
    std::memcpy(rcl_msg.buffer, raw.data(), raw.size());
    rcl_msg.buffer_length = raw.size();
    ep->ros_pub->publish(serialized_msg);

    ig->rx_count.fetch_add(1, std::memory_order_relaxed);
    ig->window_msgs.fetch_add(1, std::memory_order_relaxed);
    ig->window_bytes.fetch_add(raw.size(), std::memory_order_relaxed);
  } catch (const std::exception &e) {
    ig->dropped_count.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lk(ig->endpoints_mutex);
      ig->last_error = e.what();
    }
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                         "[%s] Failed to publish ingress %s: %s",
                         agent_id_.c_str(), local_topic.c_str(), e.what());
    addLog("ERROR", "Ingress publish failed for " + local_topic + ": " + e.what());
  }
}

void ZenohBridgeNode::handlePeerStatusSample(const zenoh::Sample &sample) {
  try {
    const auto j = nlohmann::json::parse(sample.get_payload().as_string());
    AgentStatus st = j.get<AgentStatus>();
    if (st.agent_id.empty()) return;
    std::lock_guard<std::mutex> lk(peers_mutex_);
    remote_agents_[st.agent_id] = std::move(st);
    remote_last_seen_ms_[j.value("agent_id", "")] = nowEpochMs();
  } catch (const std::exception &) {
  }
}

nlohmann::json ZenohBridgeNode::buildLocalZenohInfoJson() const {
  const auto snap = buildStatusSnapshot();
  nlohmann::json effective_cfg;
  try {
    effective_cfg = nlohmann::json::parse(effective_zenoh_config_json_);
  } catch (...) {
    effective_cfg = effective_zenoh_config_json_;
  }

  std::vector<TransportInfo> transports;
  std::vector<LinkInfo> links;
#if defined(Z_FEATURE_UNSTABLE_API)
  {
    std::lock_guard<std::mutex> slk(session_mutex_);
    if (session_.has_value()) {
      try {
        for (const auto &t : session_->get_transports()) {
          TransportInfo ti;
          ti.zid = t.get_zid().to_string();
          ti.whatami = std::string(zenoh::whatami_as_str(t.get_whatami()));
          ti.is_qos = t.is_qos();
          ti.is_multicast = t.is_multicast();
#if defined(Z_FEATURE_SHARED_MEMORY)
          ti.is_shm = t.is_shm();
#endif
          transports.push_back(std::move(ti));
        }
      } catch (...) {
      }
      try {
        for (const auto &l : session_->get_links()) {
          LinkInfo li;
          li.zid = l.get_zid().to_string();
          li.src = l.get_src();
          li.dst = l.get_dst();
          li.mtu = l.get_mtu();
          li.is_streamed = l.is_streamed();
          li.interfaces = l.get_interfaces();
          if (auto g = l.get_group()) li.group = *g;
          if (auto a = l.get_auth_identifier()) li.auth_identifier = *a;
          if (auto r = l.get_reliability()) {
            li.reliability = (*r == Z_RELIABILITY_BEST_EFFORT) ? "best_effort" : "reliable";
          }
          if (auto p = l.get_priorities()) {
            li.priorities = std::to_string(p->first) + "-" + std::to_string(p->second);
          }
          links.push_back(std::move(li));
        }
      } catch (...) {
      }
    }
  }
#endif

  return {
      {"ok", true},
      {"agent_id", agent_id_},
      {"default_agent_id", default_agent_id_},
      {"zid", snap.zid},
      {"peer_zids", snap.peer_zids},
      {"router_zids", snap.router_zids},
      {"transports", transports},
      {"links", links},
      {"bridge_settings", bridge_settings_},
      {"session_config", session_cfg_},
      {"effective_config", effective_cfg},
  };
}

void ZenohBridgeNode::handleControlQuery(const zenoh::Query &query) {
  nlohmann::json reply{{"ok", false}, {"message", "Invalid request"}};
  std::optional<SessionConfig> deferred_session;
  bool deferred_session_save = true;
  std::optional<BridgeSettings> deferred_bridge_settings;
  bool deferred_bridge_save = true;

  try {
    nlohmann::json req = nlohmann::json::object();
    if (auto p = query.get_payload()) {
      const std::string body = p->get().as_string();
      if (!body.empty()) {
        req = nlohmann::json::parse(body);
      }
    }
    const std::string action = req.value("action", "get");

    if (action == "get" || action == "scan_topics") {
      if (action == "scan_topics") {
        attachEgressRosSubscriptions();
      }
      reply = nlohmann::json{
          {"ok", true},
          {"message", "OK"},
          {"status", buildStatusSnapshot()},
          {"bridge_settings", bridge_settings_},
          {"session_config", session_cfg_},
      };
    } else if (action == "get_zenoh_info") {
      reply = buildLocalZenohInfoJson();
    } else if (action == "set_session") {
      SessionConfig next_cfg = session_cfg_;
      if (req.contains("session") && req["session"].is_object()) {
        next_cfg = req["session"].get<SessionConfig>();
      }
      std::string preview;
      auto validated = buildZenohConfig(next_cfg, preview);
      (void)validated;
      deferred_session = std::move(next_cfg);
      deferred_session_save = req.value("save", true);
      reply = nlohmann::json{
          {"ok", true},
          {"message", "Remote Zenoh config validated; reconnecting session on " + agent_id_},
      };
    } else if (action == "set_bridge_settings") {
      BridgeSettings next_bs = bridge_settings_;
      if (req.contains("bridge_settings") && req["bridge_settings"].is_object()) {
        next_bs = req["bridge_settings"].get<BridgeSettings>();
      }
      deferred_bridge_settings = std::move(next_bs);
      deferred_bridge_save = req.value("save", true);
      reply = nlohmann::json{
          {"ok", true},
          {"message", "Updating bridge settings on " + agent_id_},
      };
    } else if (action == "apply" || action == "apply_and_save") {
      RuleSet rs;
      if (req.contains("preset") && req["preset"].is_string()) {
        rs = makePresetRules(req["preset"].get<std::string>());
      } else {
        if (req.contains("egress") && req["egress"].is_array()) {
          rs.egress = req["egress"].get<std::vector<EgressRule>>();
        }
        if (req.contains("ingress") && req["ingress"].is_array()) {
          rs.ingress = req["ingress"].get<std::vector<IngressRule>>();
        }
      }
      std::string msg;
      const bool persist = (action == "apply_and_save");
      const bool ok = applyRules(rs, persist, msg);
      reply = nlohmann::json{
          {"ok", ok},
          {"message", msg},
          {"saved", ok && persist && !rules_file_.empty()},
          {"status", buildStatusSnapshot()},
      };
    } else if (action == "save") {
      if (rules_file_.empty()) {
        reply = nlohmann::json{{"ok", false}, {"message", "No rules_file configured"}};
      } else {
        std::lock_guard<std::mutex> rules_lk(rules_mutex_);
        saveRulesToYamlFile(rules_file_, getCurrentRules());
        reply = nlohmann::json{{"ok", true}, {"message", "Saved to " + rules_file_}, {"saved", true}};
      }
    } else if (action == "logs") {
      reply = nlohmann::json{
          {"ok", true},
          {"agent_id", agent_id_},
          {"logs", getRecentLogs(250)},
      };
    } else if (action == "clear_logs") {
      clearLogs();
      reply = nlohmann::json{{"ok", true}, {"agent_id", agent_id_}};
    } else {
      reply = nlohmann::json{{"ok", false}, {"message", "Unknown action: " + action}};
    }
  } catch (const std::exception &e) {
    reply = nlohmann::json{{"ok", false}, {"message", std::string("Exception: ") + e.what()}};
  }

  try {
    query.reply(query.get_keyexpr(), zenoh::Bytes(reply.dump()));
  } catch (const std::exception &e) {
    RCLCPP_WARN(get_logger(), "[%s] Control reply failed: %s", agent_id_.c_str(), e.what());
  }

  // Defer session/settings reconnect off the Zenoh callback thread to avoid deadlock!
  if (deferred_session.has_value()) {
    SessionConfig cfg_copy = *deferred_session;
    std::thread([this, cfg_copy, deferred_session_save]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      if (shutting_down_.load(std::memory_order_acquire)) return;
      std::string msg;
      reconnectSession(cfg_copy, deferred_session_save, msg);
    }).detach();
  } else if (deferred_bridge_settings.has_value()) {
    BridgeSettings bs_copy = *deferred_bridge_settings;
    std::thread([this, bs_copy, deferred_bridge_save]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      if (shutting_down_.load(std::memory_order_acquire)) return;
      std::string msg;
      updateBridgeSettings(bs_copy, deferred_bridge_save, msg);
    }).detach();
  }
}

nlohmann::json ZenohBridgeNode::queryRemoteAgent(const std::string &target_agent_id,
                                                 const nlohmann::json &request_body,
                                                 uint64_t timeout_ms) {
  std::lock_guard<std::mutex> slk(session_mutex_);
  if (!session_.has_value()) {
    return {{"ok", false}, {"message", "Zenoh session not open"}};
  }

  try {
    auto opts = zenoh::Session::GetOptions::create_default();
    opts.target = zenoh::QueryTarget::Z_QUERY_TARGET_BEST_MATCHING;
    opts.timeout_ms = timeout_ms;
    opts.payload = zenoh::Bytes(request_body.dump());

    auto replies = session_->get(
        zenoh::KeyExpr(makeControlKey(domain_prefix_, target_agent_id)),
        "", zenoh::channels::FifoChannel(16), std::move(opts));

    for (auto res = replies.recv(); std::holds_alternative<zenoh::Reply>(res); res = replies.recv()) {
      const auto &rep = std::get<zenoh::Reply>(res);
      if (rep.is_ok()) {
        const std::string text = rep.get_ok().get_payload().as_string();
        return nlohmann::json::parse(text);
      }
    }
    return {{"ok", false}, {"message", "No reply from agent '" + target_agent_id + "' (timeout)"}};
  } catch (const std::exception &e) {
    return {{"ok", false}, {"message", std::string("Remote query failed: ") + e.what()}};
  }
}

void ZenohBridgeNode::onStatusTimer() {
  const int64_t now_ns = nowSteadyNs();
  double dt_sec = static_cast<double>(now_ns - last_stats_ns_) / 1e9;
  if (dt_sec < 0.05) dt_sec = 1.0;
  last_stats_ns_ = now_ns;

  {
    std::lock_guard<std::mutex> lk(state_mutex_);
    for (auto &eg : egress_list_) {
      const uint64_t msgs = eg->window_msgs.exchange(0, std::memory_order_relaxed);
      const uint64_t bytes = eg->window_bytes.exchange(0, std::memory_order_relaxed);
      eg->last_hz = static_cast<double>(msgs) / dt_sec;
      eg->last_kbps = (static_cast<double>(bytes) / 1024.0) / dt_sec;
    }
    for (auto &ig : ingress_list_) {
      const uint64_t msgs = ig->window_msgs.exchange(0, std::memory_order_relaxed);
      const uint64_t bytes = ig->window_bytes.exchange(0, std::memory_order_relaxed);
      ig->last_hz = static_cast<double>(msgs) / dt_sec;
      ig->last_kbps = (static_cast<double>(bytes) / 1024.0) / dt_sec;
    }
  }

  AgentStatus snap = buildStatusSnapshot();
  {
    std::lock_guard<std::mutex> plk(peers_mutex_);
    remote_agents_[agent_id_] = snap;
    remote_last_seen_ms_[agent_id_] = nowEpochMs();
  }

  std::lock_guard<std::mutex> slk(session_mutex_);
  if (!status_pub_.has_value()) return;
  try {
    nlohmann::json j = snap;
    status_pub_->put(zenoh::Bytes(j.dump()));
  } catch (const std::exception &e) {
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 10000,
                         "[%s] Status publish failed: %s", agent_id_.c_str(), e.what());
  }
}

AgentStatus ZenohBridgeNode::buildStatusSnapshot() const {
  AgentStatus st;
  st.schema = kProtocolSchemaVersion;
  st.agent_id = agent_id_;
  st.robot_type = robot_type_;
  st.hostname = hostname_;
  st.ros_distro = ros_distro_;
  st.zenoh_mode = session_cfg_.mode;
  st.domain_prefix = domain_prefix_;
  st.timestamp_ms = nowEpochMs();
  st.uptime_sec = std::max<int64_t>(0, (st.timestamp_ms - started_epoch_ms_) / 1000);
  {
    std::lock_guard<std::mutex> llk(log_mutex_);
    st.warn_count = warn_count_;
    st.error_count = error_count_;
  }

  {
    std::lock_guard<std::mutex> slk(session_mutex_);
    if (session_.has_value()) {
      try {
        st.zid = session_->get_zid().to_string();
        for (const auto &id : session_->get_peers_z_id()) {
          st.peer_zids.push_back(id.to_string());
        }
        for (const auto &id : session_->get_routers_z_id()) {
          st.router_zids.push_back(id.to_string());
        }
      } catch (...) {
      }
    }
  }

  std::lock_guard<std::mutex> lk(state_mutex_);
  st.local_topics = cached_local_topics_;
  st.egress.reserve(egress_list_.size());
  for (const auto &e : egress_list_) {
    EgressRule r = e->rule;
    r.resolved_type = e->resolved_type;
    r.bound_topic = e->local_topic;
    r.effective_key = e->zenoh_key;
    r.last_error = e->last_error;
    r.actual_hz = e->last_hz;
    r.kbps = e->last_kbps;
    r.tx_count = e->tx_count.load(std::memory_order_relaxed);
    r.dropped_count = e->dropped_count.load(std::memory_order_relaxed);
    r.has_subscribers = e->has_subscribers.load(std::memory_order_relaxed);
    st.total_tx_kbps += r.kbps;
    st.egress.push_back(std::move(r));
  }
  st.ingress.reserve(ingress_list_.size());
  for (const auto &i : ingress_list_) {
    IngressRule r = i->rule;
    r.actual_hz = i->last_hz;
    r.kbps = i->last_kbps;
    r.rx_count = i->rx_count.load(std::memory_order_relaxed);
    r.dropped_count = i->dropped_count.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> elk(i->endpoints_mutex);
      for (const auto &kv : i->endpoints) {
        r.active_endpoints.push_back(kv.first);
      }
      r.last_error = i->last_error;
    }
    st.total_rx_kbps += r.kbps;
    st.ingress.push_back(std::move(r));
  }
  return st;
}

// ---------------- HTTP REST API ----------------

HttpResponse ZenohBridgeNode::handleHttpRequest(const HttpRequest &req) {
  auto makeJson = [](int status, const nlohmann::json &j) {
    HttpResponse r;
    r.status = status;
    r.body = j.dump();
    return r;
  };

  std::string path = req.path;
  if (path.rfind("/api/", 0) == 0) {
    path = path.substr(4);
  }

  if (req.method == "GET" && (path == "/health" || path == "/status")) {
    const auto snap = buildStatusSnapshot();
    return makeJson(200, {
        {"ok", true},
        {"local_agent_id", agent_id_},
        {"default_agent_id", default_agent_id_},
        {"status", snap},
        {"bridge_settings", bridge_settings_},
        {"session_config", session_cfg_},
        {"rules_file", rules_file_},
    });
  }

  if (req.method == "GET" && path == "/agents") {
    const int64_t now_ms = nowEpochMs();
    nlohmann::json arr = nlohmann::json::array();
    auto local_snap = buildStatusSnapshot();
    {
      std::lock_guard<std::mutex> plk(peers_mutex_);
      remote_agents_[agent_id_] = local_snap;
      remote_last_seen_ms_[agent_id_] = now_ms;
      for (const auto &kv : remote_agents_) {
        const int64_t seen = remote_last_seen_ms_.count(kv.first) ? remote_last_seen_ms_.at(kv.first) : 0;
        const bool online = (kv.first == agent_id_) || ((now_ms - seen) < 5000);
        nlohmann::json item = kv.second;
        item["is_local"] = (kv.first == agent_id_);
        item["online"] = online;
        item["last_seen_ms"] = seen;
        arr.push_back(std::move(item));
      }
    }
    return makeJson(200, {{"ok", true}, {"local_agent_id", agent_id_}, {"agents", arr}});
  }

  if (req.method == "GET" && path == "/local/topics") {
    attachEgressRosSubscriptions();
    std::lock_guard<std::mutex> lk(state_mutex_);
    return makeJson(200, {{"ok", true}, {"topics", cached_local_topics_}});
  }

  if (req.method == "GET" && path == "/logs") {
    const std::string target = req.query_params.count("agent_id")
                                   ? req.query_params.at("agent_id")
                                   : agent_id_;
    if (target.empty() || target == agent_id_) {
      return makeJson(200, {{"ok", true}, {"agent_id", agent_id_}, {"logs", getRecentLogs(250)}});
    }
    return makeJson(200, queryRemoteAgent(target, {{"action", "logs"}}));
  }

  if (req.method == "POST" && path == "/logs/clear") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    const std::string target = body.value("agent_id", agent_id_);
    if (target.empty() || target == agent_id_) {
      clearLogs();
      return makeJson(200, {{"ok", true}, {"agent_id", agent_id_}});
    }
    return makeJson(200, queryRemoteAgent(target, {{"action", "clear_logs"}}));
  }

  if (req.method == "GET" && path == "/rules") {
    const std::string target = req.query_params.count("agent_id")
                                   ? req.query_params.at("agent_id")
                                   : agent_id_;
    if (target.empty() || target == agent_id_) {
      attachEgressRosSubscriptions();
      return makeJson(200, {
          {"ok", true},
          {"agent_id", agent_id_},
          {"status", buildStatusSnapshot()},
      });
    }
    return makeJson(200, queryRemoteAgent(target, {{"action", "scan_topics"}}));
  }

  if (req.method == "POST" && path == "/rules") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    const std::string target = body.value("agent_id",
                                          req.query_params.count("agent_id")
                                              ? req.query_params.at("agent_id")
                                              : agent_id_);
    const bool save = body.value("save", false);
    if (target.empty() || target == agent_id_) {
      RuleSet rs;
      if (body.contains("preset") && body["preset"].is_string()) {
        rs = makePresetRules(body["preset"].get<std::string>());
      } else {
        if (body.contains("egress") && body["egress"].is_array()) {
          rs.egress = body["egress"].get<std::vector<EgressRule>>();
        }
        if (body.contains("ingress") && body["ingress"].is_array()) {
          rs.ingress = body["ingress"].get<std::vector<IngressRule>>();
        }
      }
      std::string msg;
      const bool ok = applyRules(rs, save, msg);
      return makeJson(ok ? 200 : 400, {
          {"ok", ok},
          {"message", msg},
          {"saved", ok && save && !rules_file_.empty()},
          {"status", buildStatusSnapshot()},
      });
    }
    nlohmann::json rpc = body;
    rpc["action"] = save ? "apply_and_save" : "apply";
    return makeJson(200, queryRemoteAgent(target, rpc));
  }

  if (req.method == "POST" && path == "/preset") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    const std::string preset = body.value("preset", "formation");
    const std::string target = body.value("agent_id", agent_id_);
    const bool save = body.value("save", false);

    if (target == "*" || target == "all") {
      nlohmann::json results = nlohmann::json::object();
      std::vector<std::string> targets;
      {
        const int64_t now_ms = nowEpochMs();
        std::lock_guard<std::mutex> plk(peers_mutex_);
        for (const auto &kv : remote_agents_) {
          const int64_t seen = remote_last_seen_ms_.count(kv.first) ? remote_last_seen_ms_.at(kv.first) : 0;
          if (kv.first == agent_id_ || (now_ms - seen) < 5000) {
            targets.push_back(kv.first);
          }
        }
      }
      for (const auto &aid : targets) {
        if (aid == agent_id_) {
          std::string msg;
          bool ok = applyRules(makePresetRules(preset), save, msg);
          results[aid] = {{"ok", ok}, {"message", msg}};
        } else {
          results[aid] = queryRemoteAgent(aid, {
              {"action", save ? "apply_and_save" : "apply"},
              {"preset", preset},
          });
        }
      }
      return makeJson(200, {{"ok", true}, {"preset", preset}, {"results", results}});
    }

    if (target == agent_id_) {
      std::string msg;
      const bool ok = applyRules(makePresetRules(preset), save, msg);
      return makeJson(ok ? 200 : 400, {
          {"ok", ok},
          {"message", msg},
          {"status", buildStatusSnapshot()},
      });
    }
    return makeJson(200, queryRemoteAgent(target, {
        {"action", save ? "apply_and_save" : "apply"},
        {"preset", preset},
    }));
  }

  // Bridge settings (migrated from AivudaOS config panel -> Web UI, local OR remote)
  if (req.method == "POST" && path == "/bridge/settings") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    const std::string target = body.value("agent_id", agent_id_);
    const bool save = body.value("save", true);
    BridgeSettings next_bs = bridge_settings_;
    if (body.contains("bridge_settings") && body["bridge_settings"].is_object()) {
      next_bs = body["bridge_settings"].get<BridgeSettings>();
    } else {
      next_bs = body.get<BridgeSettings>();
    }
    if (target.empty() || target == agent_id_) {
      std::string msg;
      const bool ok = updateBridgeSettings(next_bs, save, msg);
      return makeJson(ok ? 200 : 400, {
          {"ok", ok},
          {"message", msg},
          {"agent_id", agent_id_},
          {"bridge_settings", bridge_settings_},
          {"status", buildStatusSnapshot()},
      });
    }
    return makeJson(200, queryRemoteAgent(target, {
        {"action", "set_bridge_settings"},
        {"save", save},
        {"bridge_settings", next_bs},
    }));
  }

  // Zenoh Engine Info (local OR remote!)
  if (req.method == "GET" && path == "/zenoh/info") {
    const std::string target = req.query_params.count("agent_id")
                                   ? req.query_params.at("agent_id")
                                   : agent_id_;
    if (target.empty() || target == agent_id_) {
      return makeJson(200, buildLocalZenohInfoJson());
    }
    return makeJson(200, queryRemoteAgent(target, {{"action", "get_zenoh_info"}}));
  }

  // Official Zenoh 1.8.0 Default Config JSON5 template
  if (req.method == "GET" && path == "/zenoh/default_config") {
    try {
      auto def = zenoh::Config::create_default();
      nlohmann::json parsed = nlohmann::json::parse(def.to_string());
      return makeJson(200, {{"ok", true}, {"default_config", parsed}});
    } catch (const std::exception &e) {
      return makeJson(500, {{"ok", false}, {"message", e.what()}});
    }
  }

  // Dry-run validate structured or raw JSON5 config
  if (req.method == "POST" && path == "/zenoh/validate_config") {
    try {
      nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
      SessionConfig test_sc;
      if (body.contains("session") && body["session"].is_object()) {
        test_sc = body["session"].get<SessionConfig>();
      } else {
        test_sc = body.get<SessionConfig>();
      }
      std::string effective_json;
      auto cfg = buildZenohConfig(test_sc, effective_json);
      (void)cfg;
      return makeJson(200, {
          {"ok", true},
          {"message", "Zenoh JSON5 configuration is valid"},
          {"effective_config", nlohmann::json::parse(effective_json)},
      });
    } catch (const std::exception &e) {
      return makeJson(400, {{"ok", false}, {"message", e.what()}});
    }
  }

  // Reconnect Zenoh Session (local OR remote!)
  if (req.method == "POST" && path == "/zenoh/session") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    const std::string target = body.value("agent_id", agent_id_);
    SessionConfig next_cfg = session_cfg_;
    if (body.contains("session") && body["session"].is_object()) {
      next_cfg = body["session"].get<SessionConfig>();
    } else {
      next_cfg = body.get<SessionConfig>();
    }
    const bool persist = body.value("save", true);

    if (target.empty() || target == agent_id_) {
      std::string msg;
      const bool ok = reconnectSession(next_cfg, persist, msg);
      return makeJson(ok ? 200 : 400, {
          {"ok", ok},
          {"message", msg},
          {"session_config", session_cfg_},
          {"status", buildStatusSnapshot()},
      });
    }
    return makeJson(200, queryRemoteAgent(target, {
        {"action", "set_session"},
        {"save", persist},
        {"session", next_cfg},
    }));
  }

  if (req.method == "POST" && path == "/zenoh/put") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    return makeJson(200, handleZenohPutApi(body));
  }

  if (req.method == "POST" && path == "/zenoh/delete") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    return makeJson(200, handleZenohDeleteApi(body));
  }

  if (req.method == "POST" && path == "/zenoh/get") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    return makeJson(200, handleZenohGetApi(body));
  }

  if (req.method == "POST" && path == "/zenoh/scout") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    return makeJson(200, handleZenohScoutApi(body));
  }

  if (req.method == "POST" && path == "/zenoh/liveliness/get") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    return makeJson(200, handleLivelinessGetApi(body));
  }

  if (req.method == "POST" && path == "/zenoh/liveliness/declare") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    return makeJson(200, handleLivelinessDeclareApi(body));
  }

  if (req.method == "POST" && path == "/zenoh/liveliness/undeclare") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    return makeJson(200, handleLivelinessUndeclareApi(body));
  }

  if (req.method == "GET" && path == "/zenoh/liveliness/tokens") {
    std::lock_guard<std::mutex> llk(liveliness_mutex_);
    std::vector<std::string> custom_keys;
    for (const auto &kv : custom_liveliness_tokens_) custom_keys.push_back(kv.first);
    std::vector<std::string> observed(observed_liveliness_keys_.begin(), observed_liveliness_keys_.end());
    return makeJson(200, {
        {"ok", true},
        {"own_token", liveliness_enabled_ ? makeLivelinessKey(domain_prefix_, agent_id_) : ""},
        {"custom_tokens", custom_keys},
        {"observed_tokens", observed},
    });
  }

  if (req.method == "POST" && path == "/zenoh/sniff/start") {
    nlohmann::json body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
    return makeJson(200, handleSniffStartApi(body));
  }

  if (req.method == "POST" && path == "/zenoh/sniff/stop") {
    return makeJson(200, handleSniffStopApi());
  }

  if (req.method == "POST" && path == "/zenoh/sniff/clear") {
    std::lock_guard<std::mutex> lk(sniff_mutex_);
    sniff_buffer_.clear();
    sniff_total_count_ = 0;
    return makeJson(200, {{"ok", true}});
  }

  if (req.method == "GET" && path == "/zenoh/sniff") {
    std::lock_guard<std::mutex> lk(sniff_mutex_);
    nlohmann::json samples = nlohmann::json::array();
    for (const auto &s : sniff_buffer_) {
      samples.push_back({
          {"timestamp_ms", s.timestamp_ms},
          {"key_expr", s.key_expr},
          {"encoding", s.encoding},
          {"payload_bytes", s.payload_bytes},
          {"payload_preview", s.payload_preview},
          {"attachment", s.attachment},
          {"kind", s.kind},
      });
    }
    return makeJson(200, {
        {"ok", true},
        {"active", sniff_active_.load()},
        {"key_expr", sniff_key_expr_},
        {"total_count", sniff_total_count_},
        {"samples", samples},
    });
  }

  return makeJson(404, {{"ok", false}, {"message", "Not found: " + req.path}});
}

nlohmann::json ZenohBridgeNode::handleZenohPutApi(const nlohmann::json &body) {
  const std::string key = trimSlashes(body.value("key_expr", ""));
  if (key.empty()) {
    return {{"ok", false}, {"message", "key_expr is required"}};
  }
  const std::string payload = body.value("payload", "");
  const std::string encoding_str = body.value("encoding", "text/plain");
  const std::string priority_str = normalizePriorityName(body.value("priority", "data"));
  const std::string congestion_str = body.value("congestion", "drop");
  const bool express = body.value("express", false);
  const std::string reliability_str = body.value("reliability", "reliable");
  const std::string locality_str = body.value("locality", "any");
  const std::string attachment = body.value("attachment", "");

  std::lock_guard<std::mutex> slk(session_mutex_);
  if (!session_.has_value()) {
    return {{"ok", false}, {"message", "Zenoh session not open"}};
  }
  try {
    auto opts = zenoh::Session::PutOptions::create_default();
    opts.priority = parsePriority(priority_str);
    opts.congestion_control = parseCongestion(congestion_str);
    opts.is_express = express;
    opts.encoding = zenoh::Encoding(encoding_str);
    opts.allowed_destination = parseLocality(locality_str);
#if defined(Z_FEATURE_UNSTABLE_API)
    opts.reliability = (reliability_str == "best_effort")
                           ? Z_RELIABILITY_BEST_EFFORT
                           : Z_RELIABILITY_RELIABLE;
#endif
    if (!attachment.empty()) {
      opts.attachment = zenoh::Bytes(attachment);
    }
    session_->put(zenoh::KeyExpr(key), zenoh::Bytes(payload), std::move(opts));
    return {{"ok", true}, {"message", "PUT sent to " + key}, {"bytes", payload.size()}};
  } catch (const std::exception &e) {
    return {{"ok", false}, {"message", std::string("PUT failed: ") + e.what()}};
  }
}

nlohmann::json ZenohBridgeNode::handleZenohDeleteApi(const nlohmann::json &body) {
  const std::string key = trimSlashes(body.value("key_expr", ""));
  if (key.empty()) {
    return {{"ok", false}, {"message", "key_expr is required"}};
  }
  const std::string priority_str = normalizePriorityName(body.value("priority", "data"));
  const std::string congestion_str = body.value("congestion", "drop");
  const bool express = body.value("express", false);
  const std::string reliability_str = body.value("reliability", "reliable");

  std::lock_guard<std::mutex> slk(session_mutex_);
  if (!session_.has_value()) {
    return {{"ok", false}, {"message", "Zenoh session not open"}};
  }
  try {
    auto opts = zenoh::Session::DeleteOptions::create_default();
    opts.priority = parsePriority(priority_str);
    opts.congestion_control = parseCongestion(congestion_str);
    opts.is_express = express;
#if defined(Z_FEATURE_UNSTABLE_API)
    opts.reliability = (reliability_str == "best_effort")
                           ? Z_RELIABILITY_BEST_EFFORT
                           : Z_RELIABILITY_RELIABLE;
#endif
    session_->delete_resource(zenoh::KeyExpr(key), std::move(opts));
    return {{"ok", true}, {"message", "DELETE sent to " + key}};
  } catch (const std::exception &e) {
    return {{"ok", false}, {"message", std::string("DELETE failed: ") + e.what()}};
  }
}

nlohmann::json ZenohBridgeNode::handleZenohGetApi(const nlohmann::json &body) {
  const std::string key = trimSlashes(body.value("key_expr", ""));
  if (key.empty()) {
    return {{"ok", false}, {"message", "key_expr is required"}};
  }
  const std::string parameters = body.value("parameters", "");
  const std::string payload = body.value("payload", "");
  const std::string target_str = body.value("target", "best_matching");
  const std::string consolidation_str = body.value("consolidation", "auto");
  const uint64_t timeout_ms = body.value("timeout_ms", uint64_t{3000});

  std::lock_guard<std::mutex> slk(session_mutex_);
  if (!session_.has_value()) {
    return {{"ok", false}, {"message", "Zenoh session not open"}};
  }
  try {
    auto opts = zenoh::Session::GetOptions::create_default();
    opts.timeout_ms = timeout_ms;
    if (target_str == "all") {
      opts.target = zenoh::QueryTarget::Z_QUERY_TARGET_ALL;
    } else if (target_str == "all_complete") {
      opts.target = zenoh::QueryTarget::Z_QUERY_TARGET_ALL_COMPLETE;
    } else {
      opts.target = zenoh::QueryTarget::Z_QUERY_TARGET_BEST_MATCHING;
    }
    if (consolidation_str == "none") {
      opts.consolidation = zenoh::QueryConsolidation(Z_CONSOLIDATION_MODE_NONE);
    } else if (consolidation_str == "monotonic") {
      opts.consolidation = zenoh::QueryConsolidation(Z_CONSOLIDATION_MODE_MONOTONIC);
    } else if (consolidation_str == "latest") {
      opts.consolidation = zenoh::QueryConsolidation(Z_CONSOLIDATION_MODE_LATEST);
    } else {
      opts.consolidation = zenoh::QueryConsolidation(Z_CONSOLIDATION_MODE_AUTO);
    }
    if (!payload.empty()) {
      opts.payload = zenoh::Bytes(payload);
    }

    auto replies = session_->get(zenoh::KeyExpr(key), parameters,
                                 zenoh::channels::FifoChannel(128), std::move(opts));
    nlohmann::json out = nlohmann::json::array();
    for (auto res = replies.recv(); std::holds_alternative<zenoh::Reply>(res); res = replies.recv()) {
      const auto &rep = std::get<zenoh::Reply>(res);
      if (rep.is_ok()) {
        const auto &sample = rep.get_ok();
        const auto raw = sample.get_payload().as_vector();
        out.push_back({
            {"ok", true},
            {"key_expr", std::string(sample.get_keyexpr().as_string_view())},
            {"encoding", sample.get_encoding().as_string()},
            {"bytes", raw.size()},
            {"payload", formatPreview(raw, 65536)},
        });
      } else {
        const auto &err = rep.get_err();
        out.push_back({
            {"ok", false},
            {"error", err.get_payload().as_string()},
        });
      }
    }
    return {{"ok", true}, {"count", out.size()}, {"replies", out}};
  } catch (const std::exception &e) {
    return {{"ok", false}, {"message", std::string("GET failed: ") + e.what()}};
  }
}

nlohmann::json ZenohBridgeNode::handleZenohScoutApi(const nlohmann::json &body) {
  const size_t timeout_ms = std::max<size_t>(200, std::min<size_t>(10000, body.value("timeout_ms", size_t{1500})));
  const std::string what_str = body.value("what", "router_peer");
  const std::string mcast_addr = body.value("multicast_address", session_cfg_.multicast_address);
  const std::string mcast_iface = body.value("multicast_interface", session_cfg_.multicast_interface);

  try {
    auto cfg = zenoh::Config::create_default();
    if (!mcast_addr.empty()) {
      tryInsertJson5(cfg, "scouting/multicast/address", nlohmann::json(mcast_addr).dump(), get_logger());
    }
    if (!mcast_iface.empty()) {
      tryInsertJson5(cfg, "scouting/multicast/interface", nlohmann::json(mcast_iface).dump(), get_logger());
    }

    auto opts = zenoh::ScoutOptions::create_default();
    opts.timeout_ms = timeout_ms;
    if (what_str == "router") {
      opts.what = zenoh::What::Z_WHAT_ROUTER;
    } else if (what_str == "peer") {
      opts.what = zenoh::What::Z_WHAT_PEER;
    } else {
      opts.what = zenoh::What::Z_WHAT_ROUTER_PEER;
    }

    std::mutex mu;
    nlohmann::json entities = nlohmann::json::array();
    std::set<std::string> seen_zids;

    zenoh::scout(
        std::move(cfg),
        [&](const zenoh::Hello &hello) {
          std::lock_guard<std::mutex> lk(mu);
          const std::string zid = hello.get_id().to_string();
          if (!seen_zids.insert(zid).second) return;
          entities.push_back({
              {"zid", zid},
              {"whatami", std::string(zenoh::whatami_as_str(hello.get_whatami()))},
              {"locators", copyHelloLocatorsSafe(hello)},
          });
        },
        []() {},
        std::move(opts));

    return {{"ok", true}, {"count", entities.size()}, {"entities", entities}};
  } catch (const std::exception &e) {
    return {{"ok", false}, {"message", std::string("Scout failed: ") + e.what()}};
  }
}

nlohmann::json ZenohBridgeNode::handleLivelinessGetApi(const nlohmann::json &body) {
  const std::string key = trimSlashes(body.value("key_expr", makeLivelinessWildcard(domain_prefix_)));
  if (key.empty()) {
    return {{"ok", false}, {"message", "key_expr is required"}};
  }
  const uint64_t timeout_ms = body.value("timeout_ms", uint64_t{2000});

  std::lock_guard<std::mutex> slk(session_mutex_);
  if (!session_.has_value()) {
    return {{"ok", false}, {"message", "Zenoh session not open"}};
  }
  try {
    auto opts = zenoh::Session::LivelinessGetOptions::create_default();
    opts.timeout_ms = timeout_ms;
    auto replies = session_->liveliness_get(zenoh::KeyExpr(key),
                                            zenoh::channels::FifoChannel(128), std::move(opts));
    nlohmann::json tokens = nlohmann::json::array();
    for (auto res = replies.recv(); std::holds_alternative<zenoh::Reply>(res); res = replies.recv()) {
      const auto &rep = std::get<zenoh::Reply>(res);
      if (rep.is_ok()) {
        tokens.push_back(std::string(rep.get_ok().get_keyexpr().as_string_view()));
      }
    }
    return {{"ok", true}, {"key_expr", key}, {"count", tokens.size()}, {"tokens", tokens}};
  } catch (const std::exception &e) {
    return {{"ok", false}, {"message", std::string("Liveliness GET failed: ") + e.what()}};
  }
}

nlohmann::json ZenohBridgeNode::handleLivelinessDeclareApi(const nlohmann::json &body) {
  const std::string key = trimSlashes(body.value("key_expr", ""));
  if (key.empty()) {
    return {{"ok", false}, {"message", "key_expr is required"}};
  }
  {
    std::lock_guard<std::mutex> llk(liveliness_mutex_);
    if (custom_liveliness_tokens_.count(key)) {
      return {{"ok", true}, {"message", "Token already declared: " + key}};
    }
  }
  std::lock_guard<std::mutex> slk(session_mutex_);
  if (!session_.has_value()) {
    return {{"ok", false}, {"message", "Zenoh session not open"}};
  }
  try {
    // Declare OUTSIDE liveliness_mutex_ because local liveliness_sub_ callback may fire synchronously!
    auto token = session_->liveliness_declare_token(zenoh::KeyExpr(key));
    {
      std::lock_guard<std::mutex> llk(liveliness_mutex_);
      custom_liveliness_tokens_.emplace(key, std::move(token));
    }
    addLog("INFO", "Declared custom liveliness token: " + key);
    return {{"ok", true}, {"message", "Declared liveliness token: " + key}};
  } catch (const std::exception &e) {
    return {{"ok", false}, {"message", std::string("Liveliness declare failed: ") + e.what()}};
  }
}

nlohmann::json ZenohBridgeNode::handleLivelinessUndeclareApi(const nlohmann::json &body) {
  const std::string key = trimSlashes(body.value("key_expr", ""));
  if (key.empty()) {
    return {{"ok", false}, {"message", "key_expr is required"}};
  }
  std::optional<zenoh::LivelinessToken> removed;
  {
    std::lock_guard<std::mutex> llk(liveliness_mutex_);
    auto it = custom_liveliness_tokens_.find(key);
    if (it == custom_liveliness_tokens_.end()) {
      return {{"ok", false}, {"message", "Token not found: " + key}};
    }
    removed.emplace(std::move(it->second));
    custom_liveliness_tokens_.erase(it);
  }
  // Destroy token OUTSIDE liveliness_mutex_ so local liveliness_sub_ DELETE callback cannot deadlock!
  removed.reset();
  addLog("INFO", "Undeclared custom liveliness token: " + key);
  return {{"ok", true}, {"message", "Undeclared liveliness token: " + key}};
}

nlohmann::json ZenohBridgeNode::handleSniffStartApi(const nlohmann::json &body) {
  const std::string key = trimSlashes(body.value("key_expr", domain_prefix_ + "/**"));
  if (key.empty()) {
    return {{"ok", false}, {"message", "key_expr is required"}};
  }

  handleSniffStopApi();

  std::lock_guard<std::mutex> slk(session_mutex_);
  if (!session_.has_value()) {
    return {{"ok", false}, {"message", "Zenoh session not open"}};
  }

  try {
    std::lock_guard<std::mutex> lk(sniff_mutex_);
    sniff_key_expr_ = key;
    sniff_buffer_.clear();
    sniff_total_count_ = 0;
    sniff_active_.store(true, std::memory_order_release);

    sniff_sub_.emplace(session_->declare_subscriber(
        zenoh::KeyExpr(key),
        [this](const zenoh::Sample &sample) {
          if (!sniff_active_.load(std::memory_order_acquire)) return;
          SniffSample ss;
          ss.timestamp_ms = nowEpochMs();
          ss.key_expr = std::string(sample.get_keyexpr().as_string_view());
          ss.encoding = sample.get_encoding().as_string();
          const auto raw = sample.get_payload().as_vector();
          ss.payload_bytes = raw.size();
          ss.payload_preview = formatPreview(raw, 200);
          ss.kind = (sample.get_kind() == Z_SAMPLE_KIND_DELETE) ? "DELETE" : "PUT";
          if (auto att = sample.get_attachment()) {
            ss.attachment = att->get().as_string();
          }
          std::lock_guard<std::mutex> blk(sniff_mutex_);
          ++sniff_total_count_;
          sniff_buffer_.push_front(std::move(ss));
          while (sniff_buffer_.size() > 100) {
            sniff_buffer_.pop_back();
          }
        },
        zenoh::closures::none));
    return {{"ok", true}, {"key_expr", key}};
  } catch (const std::exception &e) {
    sniff_active_.store(false, std::memory_order_release);
    return {{"ok", false}, {"message", std::string("Sniff start failed: ") + e.what()}};
  }
}

nlohmann::json ZenohBridgeNode::handleSniffStopApi() {
  std::lock_guard<std::mutex> lk(sniff_mutex_);
  sniff_active_.store(false, std::memory_order_release);
  sniff_sub_.reset();
  return {{"ok", true}};
}

void ZenohBridgeNode::addLog(const std::string &level, const std::string &message) {
  const int64_t now_ms = nowEpochMs();
  std::lock_guard<std::mutex> lk(log_mutex_);
  if (message == last_log_msg_ && (now_ms - last_log_ms_) < 4000) {
    return;
  }
  last_log_msg_ = message;
  last_log_ms_ = now_ms;
  if (level == "WARN") ++warn_count_;
  if (level == "ERROR") ++error_count_;
  log_buffer_.push_front(LogEntry{now_ms, level, message});
  while (log_buffer_.size() > 300) {
    log_buffer_.pop_back();
  }
}

std::vector<LogEntry> ZenohBridgeNode::getRecentLogs(size_t max_count) const {
  std::lock_guard<std::mutex> lk(log_mutex_);
  std::vector<LogEntry> out;
  const size_t n = std::min(max_count, log_buffer_.size());
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(log_buffer_[i]);
  }
  return out;
}

void ZenohBridgeNode::clearLogs() {
  std::lock_guard<std::mutex> lk(log_mutex_);
  log_buffer_.clear();
  last_log_msg_.clear();
  last_log_ms_ = 0;
  warn_count_ = 0;
  error_count_ = 0;
}

// ---------------- Helpers ----------------

rclcpp::QoS ZenohBridgeNode::resolveSubscriptionQos(
    const std::vector<rclcpp::TopicEndpointInfo> &publishers,
    const std::string &type,
    const std::string &hint,
    int depth) {
  rclcpp::QoS qos(rclcpp::KeepLast(static_cast<size_t>(std::max(1, depth))));
  if (hint == "best_effort" || hint == "sensor_data") return qos.best_effort();
  if (hint == "reliable") return qos.reliable();
  if (hint == "transient_local") return qos.reliable().transient_local();

  bool all_reliable = true;
  bool all_transient_local = true;
  for (const auto &info : publishers) {
    if (info.topic_type() != type) continue;
    const auto &profile = info.qos_profile();
    all_reliable &= (profile.reliability() == rclcpp::ReliabilityPolicy::Reliable);
    all_transient_local &= (profile.durability() == rclcpp::DurabilityPolicy::TransientLocal);
  }
  if (!all_reliable) qos.best_effort();
  if (all_transient_local) qos.transient_local();
  return qos;
}

rclcpp::QoS ZenohBridgeNode::buildPublisherQos(const std::string &qos_hint,
                                               const std::string &attachment_qos,
                                               int depth) {
  rclcpp::QoS qos(rclcpp::KeepLast(static_cast<size_t>(std::max(1, depth))));
  if (qos_hint == "transient_local") {
    return qos.reliable().transient_local();
  }
  const std::string effective = (qos_hint == "auto" && !attachment_qos.empty())
                                    ? attachment_qos
                                    : qos_hint;
  if (effective == "best_effort" || effective == "sensor_data") {
    qos.best_effort();
  } else {
    qos.reliable();
  }
  return qos;
}

zenoh::Priority ZenohBridgeNode::parsePriority(const std::string &p) {
  const std::string n = normalizePriorityName(p);
  if (n == "real_time") return Z_PRIORITY_REAL_TIME;
  if (n == "interactive_high") return Z_PRIORITY_INTERACTIVE_HIGH;
  if (n == "interactive_low") return Z_PRIORITY_INTERACTIVE_LOW;
  if (n == "data_high") return Z_PRIORITY_DATA_HIGH;
  if (n == "data_low") return Z_PRIORITY_DATA_LOW;
  if (n == "background") return Z_PRIORITY_BACKGROUND;
  return Z_PRIORITY_DATA;
}

zenoh::CongestionControl ZenohBridgeNode::parseCongestion(const std::string &c) {
  if (c == "block") return Z_CONGESTION_CONTROL_BLOCK;
#if defined(Z_FEATURE_UNSTABLE_API)
  if (c == "block_first") return Z_CONGESTION_CONTROL_BLOCK_FIRST;
#endif
  return Z_CONGESTION_CONTROL_DROP;
}

zenoh::Locality ZenohBridgeNode::parseLocality(const std::string &loc) {
  const std::string n = normalizeLocalityName(loc);
  if (n == "remote") return Z_LOCALITY_REMOTE;
  if (n == "session_local") return Z_LOCALITY_SESSION_LOCAL;
  return Z_LOCALITY_ANY;
}

bool ZenohBridgeNode::passRateLimit(std::atomic<int64_t> &last_ns, double max_freq_hz) {
  if (max_freq_hz <= 0.0) return true;
  const int64_t min_interval_ns = static_cast<int64_t>(1e9 / max_freq_hz);
  const int64_t now = nowSteadyNs();
  int64_t prev = last_ns.load(std::memory_order_relaxed);
  while (true) {
    if (prev != 0 && (now - prev) < min_interval_ns) {
      return false;
    }
    if (last_ns.compare_exchange_weak(prev, now, std::memory_order_relaxed)) {
      return true;
    }
  }
}

bool ZenohBridgeNode::passBandwidthLimit(std::atomic<int64_t> &win_start_ns,
                                         std::atomic<uint64_t> &win_bytes,
                                         size_t msg_bytes,
                                         double max_kbps) {
  if (max_kbps <= 0.0) return true;
  const int64_t now = nowSteadyNs();
  const int64_t start = win_start_ns.load(std::memory_order_relaxed);
  if (start == 0 || (now - start) >= 1000000000LL) {
    win_start_ns.store(now, std::memory_order_relaxed);
    win_bytes.store(msg_bytes, std::memory_order_relaxed);
    return true;
  }
  const uint64_t max_bytes = static_cast<uint64_t>(max_kbps * 1024.0);
  const uint64_t used = win_bytes.fetch_add(msg_bytes, std::memory_order_relaxed) + msg_bytes;
  return used <= max_bytes;
}

int64_t ZenohBridgeNode::nowSteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t ZenohBridgeNode::nowEpochMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string ZenohBridgeNode::formatPreview(const std::vector<uint8_t> &bytes, size_t max_len) {
  if (bytes.empty()) return "";
  size_t printable = 0;
  const size_t check_n = std::min<size_t>(bytes.size(), 64);
  for (size_t i = 0; i < check_n; ++i) {
    const uint8_t c = bytes[i];
    if ((c >= 32 && c <= 126) || c == '\n' || c == '\r' || c == '\t') {
      ++printable;
    }
  }
  if (printable * 100 >= check_n * 85) {
    const size_t n = std::min(bytes.size(), max_len);
    std::string s(reinterpret_cast<const char *>(bytes.data()), n);
    if (bytes.size() > max_len) s += "...";
    return s;
  }
  std::ostringstream oss;
  oss << "<binary " << bytes.size() << "B: ";
  const size_t n = std::min<size_t>(bytes.size(), 24);
  for (size_t i = 0; i < n; ++i) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
    if (i + 1 < n) oss << ' ';
  }
  if (bytes.size() > n) oss << " ...";
  oss << ">";
  return oss.str();
}

}  // namespace zenoh_bridge
