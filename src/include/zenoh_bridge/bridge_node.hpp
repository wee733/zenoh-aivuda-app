#ifndef ZENOH_BRIDGE_BRIDGE_NODE_HPP
#define ZENOH_BRIDGE_BRIDGE_NODE_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <zenoh.hxx>

#include "zenoh_bridge/binding_retry.hpp"
#include "zenoh_bridge/http_server.hpp"
#include "zenoh_bridge/protocol.hpp"

namespace zenoh_bridge {

// Immutable inputs read under the owner lock before rclcpp::init. The node
// never reopens these files or selects a different policy after DDS starts.
struct StartupConfig {
  std::string runtime_path, settings_path, shield_script;
  nlohmann::json runtime = nlohmann::json::object();
  nlohmann::json persisted = nlohmann::json::object();
  BridgeSettings shield_policy;
  nlohmann::json shield_status;
  std::string shield_error;
  int domain_id{0};
};

class ZenohBridgeNode : public rclcpp::Node {
public:
  ZenohBridgeNode(const StartupConfig &startup, const rclcpp::NodeOptions &options);
  ~ZenohBridgeNode() override;

  RuleSet getCurrentRules() const;
  bool applyRules(const RuleSet &new_rules, bool persist_to_file, std::string &out_msg);

private:
  struct ActiveEgress {
    EgressRule rule;
    std::string rel_topic;
    std::string agent_id;  // Identity used to declare this rule's Zenoh publisher.
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
    BindingRetry binding_retry;
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
    std::string agent_id, domain_prefix;  // Identity of this subscription generation.
    std::string zenoh_pattern;
    std::optional<zenoh::Subscriber<void>> z_sub;

    // Distinct peers and ROS endpoints can reuse one custom key.
    using DemandBinding = std::tuple<std::string, std::string, std::string, std::string>;
    std::map<DemandBinding, zenoh::Subscriber<void>> demand_subs;
    mutable std::mutex endpoints_mutex;
    std::unordered_map<std::string, std::shared_ptr<IngressEndpoint>> endpoints;
    std::map<std::string, BindingRetry> binding_retries;
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

  // Published as one immutable generation so callbacks can read settings and
  // inspect the matching session without taking locks used during reconnect.
  struct RuntimeConfig {
    BridgeSettings bridge;
    std::string agent_id;
    SessionConfig session;
    nlohmann::json effective;
    std::shared_ptr<zenoh::Session> active_session;
  };
  std::shared_ptr<const RuntimeConfig> configSnapshot() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
  }
  void publishConfig(RuntimeConfig next);
  void loadStartupConfig(RuntimeConfig &config, const StartupConfig &startup);
  zenoh::Config buildZenohConfig(const SessionConfig &sc, std::string &out_effective_json) const;
  void initZenohSession(RuntimeConfig next);
  void teardownZenohSession();
  bool reconnectSession(const SessionConfig &new_cfg, bool persist, std::string &out_msg);
  // Returns the HTTP-equivalent status. Only a local authenticated request can
  // authorize a change of shield policy (or explicitly reconcile it).
  int updateBridgeSettings(const nlohmann::json &patch, bool persist, bool shield_admin,
                           std::string &out_msg, bool reconcile_shield = false,
                           const nlohmann::json &expected_policy = nullptr);
  void loadInitialRules();
  bool applyRulesLocked(const RuleSet &rules, bool persist, std::string &message);
  HttpResponse handleConfigTransfer(const HttpRequest &req, bool importing);
  std::shared_ptr<ActiveIngress> makeIngress(const IngressRule &rule, const RuntimeConfig &config);
  std::string localIngressTopic(const IngressRule &rule, const std::string &source,
                                const std::string &topic) const;
  void attachEgressRosSubscriptions();
  void onDiscoveryTimer();
  void refreshDemandIngress();
  std::shared_ptr<IngressEndpoint> getOrCreateIngressEndpoint(
      const std::shared_ptr<ActiveIngress> &ig, const std::string &topic,
      const std::string &type, const std::string &source_qos);
  void onStatusTimer();
  void publishPresence();
  void observeHeartbeat(const zenoh::Sample &sample);
  HttpResponse openTelemetryStream(const std::string &expressions);
  bool heartbeat_enabled_{true};
  bool comm_status_enabled_{true};
  rclcpp::TimerBase::SharedPtr presence_timer_;
  std::optional<zenoh::Subscriber<void>> heartbeat_sub_;
  struct Neighbor {
    std::string hostname, robot_type;
    int64_t received_ns, received_ms;
  };
  std::mutex neighbors_mutex_;
  std::map<std::string, Neighbor> neighbors_;
  std::mutex stream_slots_mutex_;
  std::array<std::weak_ptr<void>, 16> stream_slots_;
  RuleSet snapshotRulesLocked() const;
  static void readSessionIds(const std::shared_ptr<zenoh::Session> &session, AgentStatus &status);
  AgentStatus buildStatusSnapshot(const RuntimeConfig &config) const;
  bool reconnectSessionLocked(RuntimeConfig next, bool persist, std::string &out_msg,
                              const RuleSet *replacement_rules = nullptr);

  void handleRosMessage(const std::shared_ptr<ActiveEgress> &eg,
                        const std::shared_ptr<const rclcpp::SerializedMessage> &msg,
                        const std::string &attachment);
  void handleZenohSample(const std::shared_ptr<ActiveIngress> &ig,
                         const zenoh::Sample &sample,
                         const std::shared_ptr<IngressEndpoint> &offered = nullptr,
                         const std::string &offered_source = "");
  void handleControlQuery(const zenoh::Query &query);
  void handlePeerStatusSample(const zenoh::Sample &sample);

  // Local Zenoh info snapshot (including Transports & Links)
  nlohmann::json buildLocalZenohInfoJson() const;

  // Built-in DDS Shield (non-lo DDS anti-leak firewall)
  bool runDdsShieldAction(const std::string &action,
                          std::string &out_msg,
                          const std::string &scope,
                          const std::string &domains,
                          BridgeSettings *installed = nullptr);
  nlohmann::json refreshDdsShieldStatus();
  void pollDdsShieldStatus();
  std::string resolveShieldDomains(const std::string &configured_domains) const;
  nlohmann::json buildLocalDdsShieldJson() const;

  // Remote RPC via Zenoh Queryable
  nlohmann::json queryRemoteAgent(const std::string &target_agent_id,
                                  const nlohmann::json &request_body,
                                  uint64_t timeout_ms = 4000);

  // HTTP REST API for Built-in Web UI
  HttpResponse handleHttpRequest(const HttpRequest &req);
  nlohmann::json handleZenohPutApi(const nlohmann::json &body);
  nlohmann::json handleZenohDeleteApi(const nlohmann::json &body);
  nlohmann::json handleZenohGetApi(const nlohmann::json &body, uint64_t timeout_ms);
  nlohmann::json handleZenohScoutApi(const nlohmann::json &body);
  nlohmann::json handleLivelinessGetApi(const nlohmann::json &body, uint64_t timeout_ms);
  nlohmann::json handleLivelinessDeclareApi(const nlohmann::json &body);
  nlohmann::json handleLivelinessUndeclareApi(const nlohmann::json &body);
  nlohmann::json handleSniffStartApi(const nlohmann::json &body);
  nlohmann::json handleSniffStopApi();
  void addLog(const std::string &level, const std::string &message, bool coalesce = true);
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
  static std::string formatPreview(const zenoh::Bytes &payload, size_t max_len = 256);

  // Identity & configuration
  std::string default_agent_id_{"uav1"};
  std::string robot_type_;
  std::string hostname_;
  std::string avahi_hostname_;
  std::string ros_distro_;
  std::string session_override_file_;
  std::string bridge_settings_file_;
  std::string rules_file_;
  std::string dds_shield_script_;
  int ros_domain_id_{-1};
  std::string api_bind_host_{"127.0.0.1"};
  int api_port_{17447};
  int64_t started_epoch_ms_{0};

  rclcpp::CallbackGroup::SharedPtr shield_callback_group_;
  rclcpp::TimerBase::SharedPtr shield_timer_;
  // DDS Shield cached telemetry
  mutable std::mutex dds_shield_mutex_;
  bool dds_shield_active_{false};
  uint64_t dds_shield_rx_dropped_{0};
  uint64_t dds_shield_tx_dropped_{0};
  std::string dds_shield_error_;
  int64_t last_shield_poll_ns_{0};

  mutable std::mutex config_mutex_;
  std::shared_ptr<const RuntimeConfig> config_;
  // Only configuration writers take this lock, never inline Zenoh callbacks.
  std::mutex reconfigure_mutex_;

  // Zenoh runtime
  mutable std::mutex session_mutex_;
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
  uint64_t warn_count_{0};
  uint64_t error_count_{0};

  // Discovered remote agents from <prefix>/meta/*/status
  mutable std::mutex peers_mutex_;
  std::map<std::string, AgentStatus> remote_agents_;
  std::map<std::string, int64_t> remote_last_seen_ms_;

  // Rules and local ROS graph
  mutable std::mutex state_mutex_;
  std::mutex discovery_mutex_;
  std::vector<std::shared_ptr<ActiveEgress>> egress_list_;
  std::vector<std::shared_ptr<ActiveIngress>> ingress_list_;
  std::set<std::string> ingress_owned_topics_;
  std::vector<LocalTopicInfo> cached_local_topics_;
  int64_t last_stats_ns_{0};

  rclcpp::TimerBase::SharedPtr discovery_timer_;
  rclcpp::TimerBase::SharedPtr status_timer_;
  MiniHttpServer http_server_;
};

}  // namespace zenoh_bridge

#endif  // ZENOH_BRIDGE_BRIDGE_NODE_HPP
