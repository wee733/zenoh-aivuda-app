#ifndef ZENOH_BRIDGE_BRIDGE_NODE_HPP
#define ZENOH_BRIDGE_BRIDGE_NODE_HPP

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <zenoh.hxx>

#include "zenoh_bridge/http_server.hpp"
#include "zenoh_bridge/protocol.hpp"

namespace zenoh_bridge {

class ZenohBridgeNode : public rclcpp::Node {
public:
  explicit ZenohBridgeNode(const rclcpp::NodeOptions &options = rclcpp::NodeOptions());
  ~ZenohBridgeNode() override;

  RuleSet getCurrentRules() const;
  bool applyRules(const RuleSet &new_rules, bool persist_to_file, std::string &out_msg);
  AgentStatus buildStatusSnapshot() const;

private:
  struct ActiveEgress {
    EgressRule rule;
    std::string rel_topic;
    std::string local_topic;
    std::string resolved_type;
    std::atomic<uint64_t> binding_generation{0};
    std::string zenoh_key;

    rclcpp::GenericSubscription::SharedPtr ros_sub;
    std::optional<zenoh::Publisher> z_pub;
    std::optional<zenoh::MatchingListener<void>> matching_listener;

    std::atomic<bool> active{true};
    std::atomic<bool> has_subscribers{false};
    std::atomic<int64_t> last_sent_ns{0};
    std::atomic<int64_t> bw_window_start_ns{0};
    std::atomic<uint64_t> bw_window_bytes{0};
    std::atomic<uint64_t> tx_count{0};
    std::atomic<uint64_t> dropped_count{0};
    std::atomic<uint64_t> window_msgs{0};
    std::atomic<uint64_t> window_bytes{0};
    double last_hz{0.0};
    double last_kbps{0.0};
    std::string last_error;
  };

  struct IngressEndpoint {
    std::string local_topic;
    std::string msg_type;
    rclcpp::GenericPublisher::SharedPtr ros_pub;
    std::atomic<int64_t> last_rx_ns{0};
  };

  struct ActiveIngress {
    IngressRule rule;
    std::string zenoh_pattern;
    std::optional<zenoh::Subscriber<void>> z_sub;

    mutable std::mutex endpoints_mutex;
    std::unordered_map<std::string, std::shared_ptr<IngressEndpoint>> endpoints;
    std::string last_error;

    std::atomic<bool> active{true};
    std::atomic<uint64_t> rx_count{0};
    std::atomic<uint64_t> dropped_count{0};
    std::atomic<uint64_t> window_msgs{0};
    std::atomic<uint64_t> window_bytes{0};
    double last_hz{0.0};
    double last_kbps{0.0};
  };

  struct SniffSample {
    int64_t timestamp_ms{0};
    std::string key_expr;
    std::string encoding;
    size_t payload_bytes{0};
    std::string payload_preview;
    std::string attachment;
    std::string kind;
  };

  void loadRuntimeConfigFile();
  zenoh::Config buildZenohConfig(const SessionConfig &sc, std::string &out_effective_json) const;
  void initZenohSession();
  void teardownZenohSession();
  bool reconnectSession(const SessionConfig &new_cfg, bool persist, std::string &out_msg);
  bool updateBridgeSettings(const BridgeSettings &new_settings, bool persist, std::string &out_msg);
  void loadInitialRules();
  std::shared_ptr<ActiveIngress> makeIngress(const IngressRule &rule, bool initially_active = true);
  std::string localIngressTopic(const IngressRule &rule, const std::string &source,
                                const std::string &topic) const;
  void attachEgressRosSubscriptions();
  void onDiscoveryTimer();
  void onStatusTimer();

  void handleRosMessage(const std::shared_ptr<ActiveEgress> &eg,
                        const std::shared_ptr<const rclcpp::SerializedMessage> &msg,
                        const std::string &attachment);
  void handleZenohSample(const std::shared_ptr<ActiveIngress> &ig,
                         const zenoh::Sample &sample);
  void handleControlQuery(const zenoh::Query &query);
  void handlePeerStatusSample(const zenoh::Sample &sample);

  // Local Zenoh info snapshot (including Transports & Links)
  nlohmann::json buildLocalZenohInfoJson() const;

  // Remote RPC via Zenoh Queryable
  nlohmann::json queryRemoteAgent(const std::string &target_agent_id,
                                  const nlohmann::json &request_body,
                                  uint64_t timeout_ms = 4000);

  // HTTP REST API for Built-in Web UI
  HttpResponse handleHttpRequest(const HttpRequest &req);
  nlohmann::json handleZenohPutApi(const nlohmann::json &body);
  nlohmann::json handleZenohDeleteApi(const nlohmann::json &body);
  nlohmann::json handleZenohGetApi(const nlohmann::json &body);
  nlohmann::json handleZenohScoutApi(const nlohmann::json &body);
  nlohmann::json handleLivelinessGetApi(const nlohmann::json &body);
  nlohmann::json handleLivelinessDeclareApi(const nlohmann::json &body);
  nlohmann::json handleLivelinessUndeclareApi(const nlohmann::json &body);
  nlohmann::json handleSniffStartApi(const nlohmann::json &body);
  nlohmann::json handleSniffStopApi();
  void addLog(const std::string &level, const std::string &message);
  std::vector<LogEntry> getRecentLogs(size_t max_count = 200) const;
  void clearLogs();

  static rclcpp::QoS resolveSubscriptionQos(const std::vector<rclcpp::TopicEndpointInfo> &publishers,
                                            const std::string &type,
                                            const std::string &hint,
                                            int depth);
  static rclcpp::QoS buildPublisherQos(const std::string &qos_hint,
                                       const std::string &attachment_qos,
                                       int depth);
  static zenoh::Priority parsePriority(const std::string &p);
  static zenoh::CongestionControl parseCongestion(const std::string &c);
  static zenoh::Locality parseLocality(const std::string &loc);
  static bool passRateLimit(std::atomic<int64_t> &last_ns, double max_freq_hz);
  static bool passBandwidthLimit(std::atomic<int64_t> &win_start_ns,
                                 std::atomic<uint64_t> &win_bytes,
                                 size_t msg_bytes,
                                 double max_kbps);
  static int64_t nowSteadyNs();
  static int64_t nowEpochMs();
  static std::string formatPreview(const std::vector<uint8_t> &bytes, size_t max_len = 256);

  // Identity & configuration
  std::string default_agent_id_{"uav1"};
  std::string agent_id_;
  std::string robot_type_;
  std::string hostname_;
  std::string domain_prefix_;
  std::string ros_distro_;
  std::string runtime_config_file_;
  std::string session_override_file_;
  std::string bridge_settings_file_;
  std::string rules_file_;
  std::string default_preset_;
  std::string api_bind_host_{"127.0.0.1"};
  int api_port_{17447};
  bool allow_unprefixed_local_{true};
  bool liveliness_enabled_{true};
  int64_t started_epoch_ms_{0};

  BridgeSettings bridge_settings_;
  SessionConfig session_cfg_;
  std::string effective_zenoh_config_json_;

  // Zenoh runtime
  mutable std::mutex session_mutex_;
  std::optional<zenoh::Session> session_;
  std::optional<zenoh::Queryable<void>> ctrl_queryable_;
  std::optional<zenoh::Publisher> status_pub_;
  std::optional<zenoh::Subscriber<void>> swarm_status_sub_;
  std::optional<zenoh::LivelinessToken> own_liveliness_token_;
  std::optional<zenoh::Subscriber<void>> liveliness_sub_;
#if defined(Z_FEATURE_UNSTABLE_API)
  std::optional<zenoh::TransportEventsListener<void>> transport_events_listener_;
  std::optional<zenoh::LinkEventsListener<void>> link_events_listener_;
#endif

  // Custom liveliness tokens declared via Lab & observed alive keys
  mutable std::mutex liveliness_mutex_;
  std::map<std::string, zenoh::LivelinessToken> custom_liveliness_tokens_;
  std::set<std::string> observed_liveliness_keys_;

  // Live sniff/watch subscriber for Zenoh Lab
  mutable std::mutex sniff_mutex_;
  std::optional<zenoh::Subscriber<void>> sniff_sub_;
  std::string sniff_key_expr_;
  std::atomic<bool> sniff_active_{false};
  std::deque<SniffSample> sniff_buffer_;
  uint64_t sniff_total_count_{0};

  // Live log ring buffer
  mutable std::mutex log_mutex_;
  std::deque<LogEntry> log_buffer_;
  std::string last_log_msg_;
  int64_t last_log_ms_{0};
  uint64_t warn_count_{0};
  uint64_t error_count_{0};

  // Discovered remote agents from <prefix>/meta/*/status
  mutable std::mutex peers_mutex_;
  std::map<std::string, AgentStatus> remote_agents_;
  std::map<std::string, int64_t> remote_last_seen_ms_;

  // Rules and local ROS graph
  mutable std::mutex state_mutex_;
  std::mutex rules_mutex_;
  std::mutex discovery_mutex_;
  std::vector<std::shared_ptr<ActiveEgress>> egress_list_;
  std::vector<std::shared_ptr<ActiveIngress>> ingress_list_;
  std::set<std::string> ingress_owned_topics_;
  std::vector<LocalTopicInfo> cached_local_topics_;
  int64_t last_stats_ns_{0};

  rclcpp::TimerBase::SharedPtr discovery_timer_;
  rclcpp::TimerBase::SharedPtr status_timer_;
  MiniHttpServer http_server_;
  std::atomic<bool> shutting_down_{false};
};

}  // namespace zenoh_bridge

#endif  // ZENOH_BRIDGE_BRIDGE_NODE_HPP
