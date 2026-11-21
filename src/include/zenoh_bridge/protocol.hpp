#ifndef ZENOH_BRIDGE_PROTOCOL_HPP
#define ZENOH_BRIDGE_PROTOCOL_HPP

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

namespace zenoh_bridge {

inline constexpr int kProtocolSchemaVersion = 3;
inline constexpr const char *kDefaultDomainPrefix = "swarm";

struct BridgeSettings {
  std::string agent_id;                     // empty -> inherit default uav<sys.role.id>
  std::string domain_prefix{"swarm"};       // shared Zenoh key prefix
  std::string default_preset{"empty"};      // empty | formation | uxrce_swarm | vision_debug | low_bandwidth
  bool allow_unprefixed_local{true};        // match both /<agent_id>/<topic> and /<topic>
  bool liveliness_enabled{true};            // auto-declare <prefix>/alive/<agent_id> token
  bool dds_shield_enabled{false};           // opt-in: block non-lo DDS for selected domain(s) or all
  std::string dds_shield_scope{"domain"};   // domain (targeted RTPS ports only) | all (all DDS multicast + domains)
  std::string dds_shield_domains;           // comma-separated domain IDs (empty -> current ROS_DOMAIN_ID / 0)
};

struct SessionConfig {
  // Mode: "structured" (visual UI + custom_json5 merge) | "raw_json5" (full official JSON5 string)
  std::string config_mode{"structured"};
  std::string raw_json5;

  // Identity, Mode & Endpoints
  std::string custom_zid;                   // official "id" (hex ZID, empty = auto)
  std::string zenoh_namespace;              // official "namespace" (empty = none)
  std::string metadata_json5;               // official "metadata" (optional JSON5 object)
  std::string mode{"peer"};                 // peer | client | router
  std::vector<std::string> connect_endpoints;
  int connect_timeout_ms{0};                // 0 = Zenoh default
  bool connect_exit_on_failure{false};
  std::vector<std::string> listen_endpoints{"tcp/[::]:0"};
  int listen_timeout_ms{0};                 // 0 = Zenoh default
  bool listen_exit_on_failure{false};
  int queries_default_timeout_ms{10000};

  // Scouting (Discovery)
  int scouting_timeout_ms{3000};
  int scouting_delay_ms{500};
  bool multicast_enabled{true};
  std::string multicast_address{"224.0.0.224:7446"};
  std::string multicast_interface{"auto"};
  int multicast_ttl{1};
  bool multicast_listen{true};
  bool gossip_enabled{true};
  bool gossip_multihop{false};

  // Transport & Link Tuning
  bool unicast_lowlatency{false};
  bool unicast_qos_enabled{true};
  bool compression_enabled{false};
  int unicast_max_sessions{1000};
  int unicast_max_links{1};
  int unicast_open_timeout{10000};
  int unicast_accept_timeout{10000};
  bool multicast_qos_enabled{false};
  bool multicast_compression_enabled{false};
  int multicast_join_interval{2500};
  int lease_ms{10000};
  int keep_alive{4};
  int tx_batch_size{65535};
  int tx_threads{0};                        // 0 = Zenoh default
  bool batching_enabled{true};
  int batching_time_limit_ms{1};
  int rx_buffer_size{65535};
  int rx_max_message_size{1073741824};
  int tcp_so_sndbuf{0};                     // 0 = OS default
  int tcp_so_rcvbuf{0};                     // 0 = OS default
  std::string tx_queue_json5;               // optional JSON5 for transport/link/tx/queue

  // Shared Memory (SHM)
  bool shared_memory_enabled{false};
  std::string shm_mode{"lazy"};             // lazy | init
  bool shm_opt_enabled{true};
  int shm_pool_size{16777216};
  int shm_message_threshold{3072};

  // Timestamping
  bool timestamping_enabled{false};
  bool drop_future_timestamp{false};

  // Security: TLS / mTLS / Auth / ACL
  std::string tls_root_ca;
  std::string tls_listen_key;
  std::string tls_listen_cert;
  bool tls_enable_mtls{false};
  std::string tls_connect_key;
  std::string tls_connect_cert;
  bool tls_verify_name{true};
  std::string auth_user;
  std::string auth_password;
  std::string auth_dictionary_file;
  std::string auth_pubkey_file;
  std::string auth_privkey_file;
  std::string auth_known_keys_file;
  bool acl_enabled{false};
  std::string acl_default_permission{"deny"};
  std::string acl_json5;                    // optional JSON5 object with rules/subjects/policies

  // Routing, Kernel Flow Control, AdminSpace & Plugins
  bool peers_failover_brokering{true};
  std::string peer_routing_mode{"peer_to_peer"}; // peer_to_peer | linkstate
  bool adminspace_enabled{false};
  bool adminspace_read{true};
  bool adminspace_write{false};
  std::string downsampling_json5;           // JSON5 array
  std::string low_pass_filter_json5;        // JSON5 array
  std::string qos_publication_json5;        // JSON5 array
  std::string qos_network_json5;            // JSON5 array
  std::string aggregation_json5;            // JSON5 object
  bool plugins_loading_enabled{false};
  std::string plugins_json5;                // JSON5 object

  // Arbitrary true JSON5 override merged via Zenoh native parser
  std::string custom_json5;
};

struct EgressRule {
  std::string topic;
  std::string type;
  std::string zenoh_key;            // optional override; empty -> <prefix>/data/<agent>/<rel_topic>
  double max_freq{30.0};            // Hz, <=0 means unlimited
  double max_kbps{0.0};             // KB/s cap, <=0 means unlimited
  bool enabled{true};
  bool on_demand{false};
  std::string priority{"data_high"}; // real_time | interactive_high | interactive_low | data_high | data | data_low | background
  std::string congestion{"drop"};    // drop | block | block_first
  bool express{false};               // Zenoh is_express (bypass batching for low latency)
  std::string reliability{"reliable"}; // reliable | best_effort
  std::string locality{"any"};       // any | remote | session_local
  std::string qos{"auto"};           // auto | best_effort | reliable | sensor_data | transient_local
  int history_depth{10};

  // Live runtime telemetry (read-only when reported in status)
  double actual_hz{0.0};
  double kbps{0.0};
  uint64_t tx_count{0};
  uint64_t dropped_count{0};
  bool has_subscribers{false};
  std::string resolved_type;
  std::string bound_topic;
  std::string effective_key;
  std::string last_error;
};

struct IngressRule {
  std::string pattern;              // e.g. "*/fmu/out/vehicle_odometry" or "uav2/**"
  std::string remap_prefix;         // optional local prefix remap; empty -> /<src_agent>/<rel_topic>
  bool exclude_self{true};
  double max_freq{30.0};
  bool enabled{true};
  std::string locality{"any"};      // any | remote | session_local
  std::string qos{"auto"};          // auto | best_effort | reliable | transient_local
  int history_depth{10};

  // Live runtime telemetry (read-only when reported in status)
  double actual_hz{0.0};
  double kbps{0.0};
  uint64_t rx_count{0};
  uint64_t dropped_count{0};
  std::vector<std::string> active_endpoints;
  std::string last_error;
};

struct LogEntry {
  int64_t timestamp_ms{0};
  std::string level;   // INFO | WARN | ERROR
  std::string message;
};

struct LocalTopicInfo {
  std::string name;
  std::string type;
  int pub_count{0};
  int sub_count{0};
  std::string reliability;
  std::string durability;
};

struct TransportInfo {
  std::string zid;
  std::string whatami;
  bool is_qos{false};
  bool is_multicast{false};
  bool is_shm{false};
};

struct LinkInfo {
  std::string zid;
  std::string src;
  std::string dst;
  uint16_t mtu{0};
  bool is_streamed{false};
  std::vector<std::string> interfaces;
  std::string group;
  std::string auth_identifier;
  std::string reliability;
  std::string priorities;
};

struct AgentStatus {
  int schema{kProtocolSchemaVersion};
  std::string agent_id;
  std::string robot_type;
  std::string hostname;
  std::string ros_distro;
  std::string zenoh_mode{"peer"};
  std::string domain_prefix{kDefaultDomainPrefix};
  std::string zid;
  std::vector<std::string> peer_zids;
  std::vector<std::string> router_zids;
  int64_t timestamp_ms{0};
  int64_t uptime_sec{0};
  double total_tx_kbps{0.0};
  double total_rx_kbps{0.0};
  uint64_t warn_count{0};
  uint64_t error_count{0};
  bool dds_shield_enabled{false};
  bool dds_shield_active{false};
  std::string dds_shield_scope{"domain"};
  std::string dds_shield_domains;
  uint64_t dds_shield_rx_dropped{0};
  uint64_t dds_shield_tx_dropped{0};
  std::string dds_shield_error;
  std::vector<LocalTopicInfo> local_topics;
  std::vector<EgressRule> egress;
  std::vector<IngressRule> ingress;
};

struct RuleSet {
  std::vector<EgressRule> egress;
  std::vector<IngressRule> ingress;
};

// ---------------- Key Helpers ----------------

inline std::string trimSlashes(std::string s) {
  while (!s.empty() && s.front() == '/') {
    s.erase(s.begin());
  }
  while (!s.empty() && s.back() == '/') {
    s.pop_back();
  }
  return s;
}

inline std::string normalizeRelativeTopic(const std::string &topic,
                                          const std::string &agent_id = "") {
  std::string rel = trimSlashes(topic);
  if (!agent_id.empty()) {
    const std::string prefix = agent_id + "/";
    if (rel.rfind(prefix, 0) == 0) {
      rel.erase(0, prefix.size());
    }
  }
  return rel;
}

inline std::string normalizePattern(const std::string &pattern,
                                    const std::string &domain_prefix = kDefaultDomainPrefix) {
  std::string p = trimSlashes(pattern);
  const std::string data_prefix = trimSlashes(domain_prefix) + "/data/";
  if (p.rfind(data_prefix, 0) == 0) {
    p.erase(0, data_prefix.size());
  }
  return p;
}

inline std::string normalizePriorityName(const std::string &p) {
  if (p == "realtime" || p == "real_time") return "real_time";
  if (p == "interactive_high") return "interactive_high";
  if (p == "interactive_low") return "interactive_low";
  if (p == "high" || p == "data_high") return "data_high";
  if (p == "normal" || p == "data") return "data";
  if (p == "low" || p == "data_low") return "data_low";
  if (p == "background") return "background";
  return "data";
}

inline std::string normalizeLocalityName(const std::string &loc) {
  if (loc == "remote" || loc == "session_remote") return "remote";
  if (loc == "session_local" || loc == "local") return "session_local";
  return "any";
}

inline std::string makeDataKey(const std::string &domain_prefix,
                               const std::string &agent_id,
                               const std::string &rel_topic,
                               const std::string &custom_key = "") {
  if (!custom_key.empty()) {
    return trimSlashes(custom_key);
  }
  return trimSlashes(domain_prefix) + "/data/" + trimSlashes(agent_id) + "/" +
         normalizeRelativeTopic(rel_topic, agent_id);
}

inline std::string makeDataSubKey(const std::string &domain_prefix,
                                  const std::string &pattern) {
  const std::string clean = trimSlashes(pattern);
  const std::string dp = trimSlashes(domain_prefix);
  if (clean.rfind(dp + "/", 0) == 0) {
    return clean;
  }
  return dp + "/data/" + normalizePattern(clean, dp);
}

inline std::string makeStatusKey(const std::string &domain_prefix,
                                 const std::string &agent_id) {
  return trimSlashes(domain_prefix) + "/meta/" + trimSlashes(agent_id) + "/status";
}

inline std::string makeStatusWildcard(const std::string &domain_prefix) {
  return trimSlashes(domain_prefix) + "/meta/*/status";
}

inline std::string makeControlKey(const std::string &domain_prefix,
                                  const std::string &agent_id) {
  return trimSlashes(domain_prefix) + "/ctrl/" + trimSlashes(agent_id) + "/rules";
}

inline std::string makeLivelinessKey(const std::string &domain_prefix,
                                     const std::string &agent_id) {
  return trimSlashes(domain_prefix) + "/alive/" + trimSlashes(agent_id);
}

inline std::string makeLivelinessWildcard(const std::string &domain_prefix) {
  return trimSlashes(domain_prefix) + "/alive/**";
}

inline bool parseDataKey(const std::string &domain_prefix,
                         const std::string &full_key,
                         std::string &out_agent_id,
                         std::string &out_rel_topic) {
  const std::string prefix = trimSlashes(domain_prefix) + "/data/";
  if (full_key.rfind(prefix, 0) == 0) {
    const std::string rest = full_key.substr(prefix.size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
      return false;
    }
    out_agent_id = rest.substr(0, slash);
    out_rel_topic = rest.substr(slash + 1);
    return true;
  }
  const std::string clean = trimSlashes(full_key);
  const size_t slash = clean.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= clean.size()) {
    return false;
  }
  out_agent_id = clean.substr(0, slash);
  out_rel_topic = clean.substr(slash + 1);
  return true;
}

// ---------------- JSON Serialization ----------------

inline void to_json(nlohmann::json &j, const BridgeSettings &b) {
  j = nlohmann::json{
      {"agent_id", b.agent_id},
      {"domain_prefix", b.domain_prefix},
      {"default_preset", b.default_preset},
      {"allow_unprefixed_local", b.allow_unprefixed_local},
      {"liveliness_enabled", b.liveliness_enabled},
      {"dds_shield_enabled", b.dds_shield_enabled},
      {"dds_shield_scope", b.dds_shield_scope},
      {"dds_shield_domains", b.dds_shield_domains},
  };
}

// An empty list inherits the ROS domain. Otherwise require decimal CSV: never
// silently drop malformed tokens or translate an invalid policy to domain zero.
inline std::string normalizeShieldDomains(const std::string &raw) {
  const auto first = raw.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  std::set<unsigned> domains;
  for (size_t begin = 0;;) {
    const auto comma = raw.find(',', begin);
    const auto token = raw.substr(begin, comma == std::string::npos ? comma : comma - begin);
    const auto start = token.find_first_not_of(" \t");
    const auto end = token.find_last_not_of(" \t");
    if (start == std::string::npos) throw std::invalid_argument("Empty DDS Shield domain in list");
    if (end - start >= 3 || token.find_first_not_of("0123456789", start) <= end)
      throw std::invalid_argument("DDS Shield domains must be comma-separated decimal IDs in [0,232]");
    const auto id = std::stoul(token.substr(start, end - start + 1));
    if (id > 232) throw std::invalid_argument("DDS Shield domain exceeds 232");
    domains.insert(static_cast<unsigned>(id));
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
  std::string normalized;
  for (unsigned id : domains) {
    if (!normalized.empty()) normalized += ',';
    normalized += std::to_string(id);
  }
  return normalized;
}

inline void normalizeShieldPolicy(BridgeSettings &b) {
  if (b.dds_shield_scope != "domain" && b.dds_shield_scope != "all")
    throw std::invalid_argument("DDS Shield scope must be 'domain' or 'all'");
  b.dds_shield_domains = normalizeShieldDomains(b.dds_shield_domains);
}

inline void from_json(const nlohmann::json &j, BridgeSettings &b) {
  static const BridgeSettings defaults;
  b.agent_id = trimSlashes(j.value("agent_id", defaults.agent_id));
  b.domain_prefix = trimSlashes(j.value("domain_prefix", defaults.domain_prefix));
  if (b.domain_prefix.empty()) b.domain_prefix = defaults.domain_prefix;
  b.default_preset = j.value("default_preset", defaults.default_preset);
  b.allow_unprefixed_local = j.value("allow_unprefixed_local", defaults.allow_unprefixed_local);
  b.liveliness_enabled = j.value("liveliness_enabled", defaults.liveliness_enabled);
  b.dds_shield_enabled = j.value("dds_shield_enabled", defaults.dds_shield_enabled);
  b.dds_shield_scope = j.value("dds_shield_scope", defaults.dds_shield_scope);
  b.dds_shield_domains = j.value("dds_shield_domains", defaults.dds_shield_domains);
  normalizeShieldPolicy(b);
}

inline BridgeSettings mergedBridgeSettings(const BridgeSettings &base, const nlohmann::json &patch) {
  if (!patch.is_object()) throw std::invalid_argument("bridge_settings must be an object");
  nlohmann::json merged = base;
  merged.update(patch);  // Preserve absent fields; explicit null is a type error, not a reset.
  return merged.get<BridgeSettings>();
}

inline void to_json(nlohmann::json &j, const SessionConfig &c) {
  j = nlohmann::json{
      {"config_mode", c.config_mode},
      {"raw_json5", c.raw_json5},
      {"custom_zid", c.custom_zid},
      {"zenoh_namespace", c.zenoh_namespace},
      {"metadata_json5", c.metadata_json5},
      {"mode", c.mode},
      {"connect_endpoints", c.connect_endpoints},
      {"connect_timeout_ms", c.connect_timeout_ms},
      {"connect_exit_on_failure", c.connect_exit_on_failure},
      {"listen_endpoints", c.listen_endpoints},
      {"listen_timeout_ms", c.listen_timeout_ms},
      {"listen_exit_on_failure", c.listen_exit_on_failure},
      {"queries_default_timeout_ms", c.queries_default_timeout_ms},
      {"scouting_timeout_ms", c.scouting_timeout_ms},
      {"scouting_delay_ms", c.scouting_delay_ms},
      {"multicast_enabled", c.multicast_enabled},
      {"multicast_address", c.multicast_address},
      {"multicast_interface", c.multicast_interface},
      {"multicast_ttl", c.multicast_ttl},
      {"multicast_listen", c.multicast_listen},
      {"gossip_enabled", c.gossip_enabled},
      {"gossip_multihop", c.gossip_multihop},
      {"unicast_lowlatency", c.unicast_lowlatency},
      {"unicast_qos_enabled", c.unicast_qos_enabled},
      {"compression_enabled", c.compression_enabled},
      {"unicast_max_sessions", c.unicast_max_sessions},
      {"unicast_max_links", c.unicast_max_links},
      {"unicast_open_timeout", c.unicast_open_timeout},
      {"unicast_accept_timeout", c.unicast_accept_timeout},
      {"multicast_qos_enabled", c.multicast_qos_enabled},
      {"multicast_compression_enabled", c.multicast_compression_enabled},
      {"multicast_join_interval", c.multicast_join_interval},
      {"lease_ms", c.lease_ms},
      {"keep_alive", c.keep_alive},
      {"tx_batch_size", c.tx_batch_size},
      {"tx_threads", c.tx_threads},
      {"batching_enabled", c.batching_enabled},
      {"batching_time_limit_ms", c.batching_time_limit_ms},
      {"rx_buffer_size", c.rx_buffer_size},
      {"rx_max_message_size", c.rx_max_message_size},
      {"tcp_so_sndbuf", c.tcp_so_sndbuf},
      {"tcp_so_rcvbuf", c.tcp_so_rcvbuf},
      {"tx_queue_json5", c.tx_queue_json5},
      {"shared_memory_enabled", c.shared_memory_enabled},
      {"shm_mode", c.shm_mode},
      {"shm_opt_enabled", c.shm_opt_enabled},
      {"shm_pool_size", c.shm_pool_size},
      {"shm_message_threshold", c.shm_message_threshold},
      {"timestamping_enabled", c.timestamping_enabled},
      {"drop_future_timestamp", c.drop_future_timestamp},
      {"tls_root_ca", c.tls_root_ca},
      {"tls_listen_key", c.tls_listen_key},
      {"tls_listen_cert", c.tls_listen_cert},
      {"tls_enable_mtls", c.tls_enable_mtls},
      {"tls_connect_key", c.tls_connect_key},
      {"tls_connect_cert", c.tls_connect_cert},
      {"tls_verify_name", c.tls_verify_name},
      {"auth_user", c.auth_user},
      {"auth_password", c.auth_password},
      {"auth_dictionary_file", c.auth_dictionary_file},
      {"auth_pubkey_file", c.auth_pubkey_file},
      {"auth_privkey_file", c.auth_privkey_file},
      {"auth_known_keys_file", c.auth_known_keys_file},
      {"acl_enabled", c.acl_enabled},
      {"acl_default_permission", c.acl_default_permission},
      {"acl_json5", c.acl_json5},
      {"peers_failover_brokering", c.peers_failover_brokering},
      {"peer_routing_mode", c.peer_routing_mode},
      {"adminspace_enabled", c.adminspace_enabled},
      {"adminspace_read", c.adminspace_read},
      {"adminspace_write", c.adminspace_write},
      {"downsampling_json5", c.downsampling_json5},
      {"low_pass_filter_json5", c.low_pass_filter_json5},
      {"qos_publication_json5", c.qos_publication_json5},
      {"qos_network_json5", c.qos_network_json5},
      {"aggregation_json5", c.aggregation_json5},
      {"plugins_loading_enabled", c.plugins_loading_enabled},
      {"plugins_json5", c.plugins_json5},
      {"custom_json5", c.custom_json5},
  };
}

inline void from_json(const nlohmann::json &j, SessionConfig &c) {
  static const SessionConfig defaults;
  c.config_mode = j.value("config_mode", defaults.config_mode);
  c.raw_json5 = j.value("raw_json5", defaults.raw_json5);
  c.custom_zid = j.value("custom_zid", defaults.custom_zid);
  c.zenoh_namespace = j.value("zenoh_namespace", defaults.zenoh_namespace);
  c.metadata_json5 = j.value("metadata_json5", defaults.metadata_json5);
  c.mode = j.value("mode", defaults.mode);
  if (j.contains("connect_endpoints") && j["connect_endpoints"].is_array()) {
    c.connect_endpoints = j["connect_endpoints"].get<std::vector<std::string>>();
  }
  c.connect_timeout_ms = j.value("connect_timeout_ms", defaults.connect_timeout_ms);
  c.connect_exit_on_failure = j.value("connect_exit_on_failure", defaults.connect_exit_on_failure);
  if (j.contains("listen_endpoints") && j["listen_endpoints"].is_array()) {
    c.listen_endpoints = j["listen_endpoints"].get<std::vector<std::string>>();
  }
  c.listen_timeout_ms = j.value("listen_timeout_ms", defaults.listen_timeout_ms);
  c.listen_exit_on_failure = j.value("listen_exit_on_failure", defaults.listen_exit_on_failure);
  c.queries_default_timeout_ms = j.value("queries_default_timeout_ms", defaults.queries_default_timeout_ms);
  c.scouting_timeout_ms = j.value("scouting_timeout_ms", defaults.scouting_timeout_ms);
  c.scouting_delay_ms = j.value("scouting_delay_ms", defaults.scouting_delay_ms);
  c.multicast_enabled = j.value("multicast_enabled", defaults.multicast_enabled);
  c.multicast_address = j.value("multicast_address", defaults.multicast_address);
  c.multicast_interface = j.value("multicast_interface", defaults.multicast_interface);
  c.multicast_ttl = j.value("multicast_ttl", defaults.multicast_ttl);
  c.multicast_listen = j.value("multicast_listen", defaults.multicast_listen);
  c.gossip_enabled = j.value("gossip_enabled", defaults.gossip_enabled);
  c.gossip_multihop = j.value("gossip_multihop", defaults.gossip_multihop);
  c.unicast_lowlatency = j.value("unicast_lowlatency", defaults.unicast_lowlatency);
  c.unicast_qos_enabled = j.value("unicast_qos_enabled", defaults.unicast_qos_enabled);
  c.compression_enabled = j.value("compression_enabled", defaults.compression_enabled);
  c.unicast_max_sessions = j.value("unicast_max_sessions", defaults.unicast_max_sessions);
  c.unicast_max_links = j.value("unicast_max_links", defaults.unicast_max_links);
  c.unicast_open_timeout = j.value("unicast_open_timeout", defaults.unicast_open_timeout);
  c.unicast_accept_timeout = j.value("unicast_accept_timeout", defaults.unicast_accept_timeout);
  c.multicast_qos_enabled = j.value("multicast_qos_enabled", defaults.multicast_qos_enabled);
  c.multicast_compression_enabled = j.value("multicast_compression_enabled", defaults.multicast_compression_enabled);
  c.multicast_join_interval = j.value("multicast_join_interval", defaults.multicast_join_interval);
  c.lease_ms = j.value("lease_ms", defaults.lease_ms);
  c.keep_alive = j.value("keep_alive", defaults.keep_alive);
  c.tx_batch_size = j.value("tx_batch_size", defaults.tx_batch_size);
  c.tx_threads = j.value("tx_threads", defaults.tx_threads);
  c.batching_enabled = j.value("batching_enabled", defaults.batching_enabled);
  c.batching_time_limit_ms = j.value("batching_time_limit_ms", defaults.batching_time_limit_ms);
  c.rx_buffer_size = j.value("rx_buffer_size", defaults.rx_buffer_size);
  c.rx_max_message_size = j.value("rx_max_message_size", defaults.rx_max_message_size);
  c.tcp_so_sndbuf = j.value("tcp_so_sndbuf", defaults.tcp_so_sndbuf);
  c.tcp_so_rcvbuf = j.value("tcp_so_rcvbuf", defaults.tcp_so_rcvbuf);
  c.tx_queue_json5 = j.value("tx_queue_json5", defaults.tx_queue_json5);
  c.shared_memory_enabled = j.value("shared_memory_enabled", defaults.shared_memory_enabled);
  c.shm_mode = j.value("shm_mode", defaults.shm_mode);
  c.shm_opt_enabled = j.value("shm_opt_enabled", defaults.shm_opt_enabled);
  c.shm_pool_size = j.value("shm_pool_size", defaults.shm_pool_size);
  c.shm_message_threshold = j.value("shm_message_threshold", defaults.shm_message_threshold);
  c.timestamping_enabled = j.value("timestamping_enabled", defaults.timestamping_enabled);
  c.drop_future_timestamp = j.value("drop_future_timestamp", defaults.drop_future_timestamp);
  c.tls_root_ca = j.value("tls_root_ca", defaults.tls_root_ca);
  c.tls_listen_key = j.value("tls_listen_key", defaults.tls_listen_key);
  c.tls_listen_cert = j.value("tls_listen_cert", defaults.tls_listen_cert);
  c.tls_enable_mtls = j.value("tls_enable_mtls", defaults.tls_enable_mtls);
  c.tls_connect_key = j.value("tls_connect_key", defaults.tls_connect_key);
  c.tls_connect_cert = j.value("tls_connect_cert", defaults.tls_connect_cert);
  c.tls_verify_name = j.value("tls_verify_name", defaults.tls_verify_name);
  c.auth_user = j.value("auth_user", defaults.auth_user);
  c.auth_password = j.value("auth_password", defaults.auth_password);
  c.auth_dictionary_file = j.value("auth_dictionary_file", defaults.auth_dictionary_file);
  c.auth_pubkey_file = j.value("auth_pubkey_file", defaults.auth_pubkey_file);
  c.auth_privkey_file = j.value("auth_privkey_file", defaults.auth_privkey_file);
  c.auth_known_keys_file = j.value("auth_known_keys_file", defaults.auth_known_keys_file);
  c.acl_enabled = j.value("acl_enabled", defaults.acl_enabled);
  c.acl_default_permission = j.value("acl_default_permission", defaults.acl_default_permission);
  c.acl_json5 = j.value("acl_json5", defaults.acl_json5);
  c.peers_failover_brokering = j.value("peers_failover_brokering", defaults.peers_failover_brokering);
  c.peer_routing_mode = j.value("peer_routing_mode", defaults.peer_routing_mode);
  c.adminspace_enabled = j.value("adminspace_enabled", defaults.adminspace_enabled);
  c.adminspace_read = j.value("adminspace_read", defaults.adminspace_read);
  c.adminspace_write = j.value("adminspace_write", defaults.adminspace_write);
  c.downsampling_json5 = j.value("downsampling_json5", defaults.downsampling_json5);
  c.low_pass_filter_json5 = j.value("low_pass_filter_json5", defaults.low_pass_filter_json5);
  c.qos_publication_json5 = j.value("qos_publication_json5", defaults.qos_publication_json5);
  c.qos_network_json5 = j.value("qos_network_json5", defaults.qos_network_json5);
  c.aggregation_json5 = j.value("aggregation_json5", defaults.aggregation_json5);
  c.plugins_loading_enabled = j.value("plugins_loading_enabled", defaults.plugins_loading_enabled);
  c.plugins_json5 = j.value("plugins_json5", defaults.plugins_json5);
  c.custom_json5 = j.value("custom_json5", defaults.custom_json5);
}

inline void to_json(nlohmann::json &j, const EgressRule &r) {
  j = nlohmann::json{
      {"topic", r.topic},
      {"type", r.type},
      {"zenoh_key", r.zenoh_key},
      {"max_freq", r.max_freq},
      {"max_kbps", r.max_kbps},
      {"enabled", r.enabled},
      {"on_demand", r.on_demand},
      {"priority", r.priority},
      {"congestion", r.congestion},
      {"express", r.express},
      {"reliability", r.reliability},
      {"locality", r.locality},
      {"qos", r.qos},
      {"history_depth", r.history_depth},
      {"actual_hz", r.actual_hz},
      {"kbps", r.kbps},
      {"tx_count", r.tx_count},
      {"dropped_count", r.dropped_count},
      {"has_subscribers", r.has_subscribers},
      {"resolved_type", r.resolved_type},
      {"bound_topic", r.bound_topic},
      {"effective_key", r.effective_key},
      {"last_error", r.last_error},
  };
}

inline void from_json(const nlohmann::json &j, EgressRule &r) {
  r.topic = j.value("topic", "");
  r.type = j.value("type", "");
  r.zenoh_key = j.value("zenoh_key", "");
  r.max_freq = j.value("max_freq", 30.0);
  r.max_kbps = j.value("max_kbps", 0.0);
  r.enabled = j.value("enabled", true);
  r.on_demand = j.value("on_demand", false);
  r.priority = normalizePriorityName(j.value("priority", "data_high"));
  r.congestion = j.value("congestion", "drop");
  r.express = j.value("express", r.priority == "real_time" || r.priority == "interactive_high");
  r.reliability = j.value("reliability", "reliable");
  r.locality = normalizeLocalityName(j.value("locality", "any"));
  r.qos = j.value("qos", "auto");
  r.history_depth = std::max(1, std::min(1000, j.value("history_depth", 10)));
  r.actual_hz = j.value("actual_hz", 0.0);
  r.kbps = j.value("kbps", 0.0);
  r.tx_count = j.value("tx_count", uint64_t{0});
  r.dropped_count = j.value("dropped_count", uint64_t{0});
  r.has_subscribers = j.value("has_subscribers", false);
  r.resolved_type = j.value("resolved_type", "");
  r.bound_topic = j.value("bound_topic", "");
  r.effective_key = j.value("effective_key", "");
  r.last_error = j.value("last_error", "");
}

inline void to_json(nlohmann::json &j, const IngressRule &r) {
  j = nlohmann::json{
      {"pattern", r.pattern},
      {"remap_prefix", r.remap_prefix},
      {"exclude_self", r.exclude_self},
      {"max_freq", r.max_freq},
      {"enabled", r.enabled},
      {"locality", r.locality},
      {"qos", r.qos},
      {"history_depth", r.history_depth},
      {"actual_hz", r.actual_hz},
      {"kbps", r.kbps},
      {"rx_count", r.rx_count},
      {"dropped_count", r.dropped_count},
      {"active_endpoints", r.active_endpoints},
      {"last_error", r.last_error},
  };
}

inline void from_json(const nlohmann::json &j, IngressRule &r) {
  r.pattern = j.value("pattern", "");
  r.remap_prefix = j.value("remap_prefix", "");
  r.exclude_self = j.value("exclude_self", true);
  r.max_freq = j.value("max_freq", 30.0);
  r.enabled = j.value("enabled", true);
  r.locality = normalizeLocalityName(j.value("locality", "any"));
  r.qos = j.value("qos", "auto");
  r.history_depth = std::max(1, std::min(1000, j.value("history_depth", 10)));
  r.actual_hz = j.value("actual_hz", 0.0);
  r.kbps = j.value("kbps", 0.0);
  r.rx_count = j.value("rx_count", uint64_t{0});
  r.dropped_count = j.value("dropped_count", uint64_t{0});
  if (j.contains("active_endpoints") && j["active_endpoints"].is_array()) {
    r.active_endpoints = j["active_endpoints"].get<std::vector<std::string>>();
  }
  r.last_error = j.value("last_error", "");
}

inline void to_json(nlohmann::json &j, const LogEntry &e) {
  j = nlohmann::json{
      {"timestamp_ms", e.timestamp_ms},
      {"level", e.level},
      {"message", e.message},
  };
}

inline void to_json(nlohmann::json &j, const LocalTopicInfo &t) {
  j = nlohmann::json{
      {"name", t.name},
      {"type", t.type},
      {"pub_count", t.pub_count},
      {"sub_count", t.sub_count},
      {"reliability", t.reliability},
      {"durability", t.durability},
  };
}

inline void from_json(const nlohmann::json &j, LocalTopicInfo &t) {
  t.name = j.value("name", "");
  t.type = j.value("type", "");
  t.pub_count = j.value("pub_count", 0);
  t.sub_count = j.value("sub_count", 0);
  t.reliability = j.value("reliability", "");
  t.durability = j.value("durability", "");
}

inline void to_json(nlohmann::json &j, const TransportInfo &t) {
  j = nlohmann::json{
      {"zid", t.zid},
      {"whatami", t.whatami},
      {"is_qos", t.is_qos},
      {"is_multicast", t.is_multicast},
      {"is_shm", t.is_shm},
  };
}

inline void to_json(nlohmann::json &j, const LinkInfo &l) {
  j = nlohmann::json{
      {"zid", l.zid},
      {"src", l.src},
      {"dst", l.dst},
      {"mtu", l.mtu},
      {"is_streamed", l.is_streamed},
      {"interfaces", l.interfaces},
      {"group", l.group},
      {"auth_identifier", l.auth_identifier},
      {"reliability", l.reliability},
      {"priorities", l.priorities},
  };
}

inline void to_json(nlohmann::json &j, const AgentStatus &s) {
  j = nlohmann::json{
      {"schema", s.schema},
      {"agent_id", s.agent_id},
      {"robot_type", s.robot_type},
      {"hostname", s.hostname},
      {"ros_distro", s.ros_distro},
      {"zenoh_mode", s.zenoh_mode},
      {"domain_prefix", s.domain_prefix},
      {"zid", s.zid},
      {"peer_zids", s.peer_zids},
      {"router_zids", s.router_zids},
      {"timestamp_ms", s.timestamp_ms},
      {"uptime_sec", s.uptime_sec},
      {"total_tx_kbps", s.total_tx_kbps},
      {"total_rx_kbps", s.total_rx_kbps},
      {"warn_count", s.warn_count},
      {"error_count", s.error_count},
      {"dds_shield_enabled", s.dds_shield_enabled},
      {"dds_shield_active", s.dds_shield_active},
      {"dds_shield_scope", s.dds_shield_scope},
      {"dds_shield_domains", s.dds_shield_domains},
      {"dds_shield_rx_dropped", s.dds_shield_rx_dropped},
      {"dds_shield_tx_dropped", s.dds_shield_tx_dropped},
      {"dds_shield_error", s.dds_shield_error},
      {"local_topics", s.local_topics},
      {"egress", s.egress},
      {"ingress", s.ingress},
  };
}

inline void from_json(const nlohmann::json &j, AgentStatus &s) {
  s.schema = j.value("schema", kProtocolSchemaVersion);
  s.agent_id = j.value("agent_id", "");
  s.robot_type = j.value("robot_type", "");
  s.hostname = j.value("hostname", "");
  s.ros_distro = j.value("ros_distro", "");
  s.zenoh_mode = j.value("zenoh_mode", "peer");
  s.domain_prefix = j.value("domain_prefix", kDefaultDomainPrefix);
  s.zid = j.value("zid", "");
  if (j.contains("peer_zids") && j["peer_zids"].is_array()) {
    s.peer_zids = j["peer_zids"].get<std::vector<std::string>>();
  }
  if (j.contains("router_zids") && j["router_zids"].is_array()) {
    s.router_zids = j["router_zids"].get<std::vector<std::string>>();
  }
  s.timestamp_ms = j.value("timestamp_ms", int64_t{0});
  s.uptime_sec = j.value("uptime_sec", int64_t{0});
  s.total_tx_kbps = j.value("total_tx_kbps", 0.0);
  s.total_rx_kbps = j.value("total_rx_kbps", 0.0);
  s.warn_count = j.value("warn_count", uint64_t{0});
  s.error_count = j.value("error_count", uint64_t{0});
  s.dds_shield_enabled = j.value("dds_shield_enabled", false);
  s.dds_shield_active = j.value("dds_shield_active", false);
  s.dds_shield_scope = j.value("dds_shield_scope", "domain");
  s.dds_shield_domains = j.value("dds_shield_domains", "");
  s.dds_shield_rx_dropped = j.value("dds_shield_rx_dropped", uint64_t{0});
  s.dds_shield_tx_dropped = j.value("dds_shield_tx_dropped", uint64_t{0});
  s.dds_shield_error = j.value("dds_shield_error", "");
  if (j.contains("local_topics") && j["local_topics"].is_array()) {
    s.local_topics = j["local_topics"].get<std::vector<LocalTopicInfo>>();
  } else {
    s.local_topics.clear();
  }
  if (j.contains("egress") && j["egress"].is_array()) {
    s.egress = j["egress"].get<std::vector<EgressRule>>();
  } else {
    s.egress.clear();
  }
  if (j.contains("ingress") && j["ingress"].is_array()) {
    s.ingress = j["ingress"].get<std::vector<IngressRule>>();
  } else {
    s.ingress.clear();
  }
}

// ---------------- YAML Persistence ----------------

inline RuleSet loadRulesFromYamlFile(const std::string &path) {
  RuleSet rules;
  YAML::Node root = YAML::LoadFile(path);
  if (!root || !root.IsMap()) {
    return rules;
  }
  if (root["egress"] && root["egress"].IsSequence()) {
    for (const auto &node : root["egress"]) {
      EgressRule r;
      r.topic = node["topic"] ? node["topic"].as<std::string>() : "";
      r.type = node["type"] ? node["type"].as<std::string>() : "";
      r.zenoh_key = node["zenoh_key"] ? node["zenoh_key"].as<std::string>() : "";
      r.max_freq = node["max_freq"] ? node["max_freq"].as<double>() : 30.0;
      r.max_kbps = node["max_kbps"] ? node["max_kbps"].as<double>() : 0.0;
      r.enabled = node["enabled"] ? node["enabled"].as<bool>() : true;
      r.on_demand = node["on_demand"] ? node["on_demand"].as<bool>() : false;
      r.priority = normalizePriorityName(node["priority"] ? node["priority"].as<std::string>() : "data_high");
      r.congestion = node["congestion"] ? node["congestion"].as<std::string>() : "drop";
      r.express = node["express"] ? node["express"].as<bool>()
                                  : (r.priority == "real_time" || r.priority == "interactive_high");
      r.reliability = node["reliability"] ? node["reliability"].as<std::string>() : "reliable";
      r.locality = normalizeLocalityName(node["locality"] ? node["locality"].as<std::string>() : "any");
      r.qos = node["qos"] ? node["qos"].as<std::string>() : "auto";
      r.history_depth = node["history_depth"] ? std::max(1, std::min(1000, node["history_depth"].as<int>())) : 10;
      if (!r.topic.empty()) {
        rules.egress.push_back(std::move(r));
      }
    }
  }
  if (root["ingress"] && root["ingress"].IsSequence()) {
    for (const auto &node : root["ingress"]) {
      IngressRule r;
      r.pattern = node["pattern"] ? node["pattern"].as<std::string>() : "";
      r.remap_prefix = node["remap_prefix"] ? node["remap_prefix"].as<std::string>() : "";
      r.exclude_self = node["exclude_self"] ? node["exclude_self"].as<bool>() : true;
      r.max_freq = node["max_freq"] ? node["max_freq"].as<double>() : 30.0;
      r.enabled = node["enabled"] ? node["enabled"].as<bool>() : true;
      r.locality = normalizeLocalityName(node["locality"] ? node["locality"].as<std::string>() : "any");
      r.qos = node["qos"] ? node["qos"].as<std::string>() : "auto";
      r.history_depth = node["history_depth"] ? std::max(1, std::min(1000, node["history_depth"].as<int>())) : 10;
      if (!r.pattern.empty()) {
        rules.ingress.push_back(std::move(r));
      }
    }
  }
  return rules;
}

inline void atomicWriteFile(const std::string &path, const std::string &content,
                            const std::string &write_error = "Failed writing file: ",
                            const std::string &rename_error = "Failed to atomically replace file: ") {
  std::string tmp = path + ".tmp.XXXXXX";
  const auto slash = path.rfind('/');
  const auto parent = slash == std::string::npos ? "." : path.substr(0, slash + 1);
  const int dirfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dirfd < 0) throw std::runtime_error("Cannot open settings directory: " + parent);
  struct CloseDirectory {
    int fd;
    ~CloseDirectory() { ::close(fd); }
  } directory{dirfd};
  const auto sync = [](int f) {
    int rc;
    do { rc = ::fsync(f); } while (rc < 0 && errno == EINTR);
    return rc == 0;
  };
  const int fd = ::mkostemp(tmp.data(), O_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error("Failed to open temp file for writing: " + tmp);
  }
  std::size_t written = 0;
  while (written < content.size()) {
    const auto n = ::write(fd, content.data() + written, content.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    written += static_cast<std::size_t>(n);
  }
  const bool flushed = written == content.size() && sync(fd);
  if (::close(fd) != 0 || !flushed) {
    std::remove(tmp.c_str());
    throw std::runtime_error(write_error + tmp);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    throw std::runtime_error(rename_error + path);
  }
  if (!sync(dirfd)) {
    const int error = errno;
    throw std::runtime_error("Replaced " + path + ", but directory sync failed: " + std::strerror(error));
  }
}

inline void saveRulesToYamlFile(const std::string &path, const RuleSet &rules) {
  YAML::Emitter out;
  out << YAML::BeginMap;
  out << YAML::Key << "schema" << YAML::Value << kProtocolSchemaVersion;

  out << YAML::Key << "egress" << YAML::Value << YAML::BeginSeq;
  for (const auto &r : rules.egress) {
    out << YAML::BeginMap;
    out << YAML::Key << "topic" << YAML::Value << r.topic;
    if (!r.type.empty()) out << YAML::Key << "type" << YAML::Value << r.type;
    if (!r.zenoh_key.empty()) out << YAML::Key << "zenoh_key" << YAML::Value << r.zenoh_key;
    out << YAML::Key << "max_freq" << YAML::Value << r.max_freq;
    out << YAML::Key << "max_kbps" << YAML::Value << r.max_kbps;
    out << YAML::Key << "enabled" << YAML::Value << r.enabled;
    out << YAML::Key << "on_demand" << YAML::Value << r.on_demand;
    out << YAML::Key << "priority" << YAML::Value << r.priority;
    out << YAML::Key << "congestion" << YAML::Value << r.congestion;
    out << YAML::Key << "express" << YAML::Value << r.express;
    out << YAML::Key << "reliability" << YAML::Value << r.reliability;
    out << YAML::Key << "locality" << YAML::Value << r.locality;
    out << YAML::Key << "qos" << YAML::Value << r.qos;
    out << YAML::Key << "history_depth" << YAML::Value << r.history_depth;
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;

  out << YAML::Key << "ingress" << YAML::Value << YAML::BeginSeq;
  for (const auto &r : rules.ingress) {
    out << YAML::BeginMap;
    out << YAML::Key << "pattern" << YAML::Value << r.pattern;
    if (!r.remap_prefix.empty()) out << YAML::Key << "remap_prefix" << YAML::Value << r.remap_prefix;
    out << YAML::Key << "exclude_self" << YAML::Value << r.exclude_self;
    out << YAML::Key << "max_freq" << YAML::Value << r.max_freq;
    out << YAML::Key << "enabled" << YAML::Value << r.enabled;
    out << YAML::Key << "locality" << YAML::Value << r.locality;
    out << YAML::Key << "qos" << YAML::Value << r.qos;
    out << YAML::Key << "history_depth" << YAML::Value << r.history_depth;
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;
  out << YAML::EndMap;

  atomicWriteFile(path, std::string(out.c_str()) + "\n",
                  "Failed while writing rules file: ", "Failed to atomically replace rules file: ");
}

// ---------------- Built-in Presets ----------------

inline RuleSet makePresetRules(const std::string &preset_name) {
  RuleSet rs;
  if (preset_name == "empty" || preset_name == "none") {
    return rs;
  }

  if (preset_name == "uxrce_swarm") {
    rs.egress = {
        {"fmu/out/vehicle_odometry", "", "", 30.0, 0.0, true, false, "data_high", "drop", true, "best_effort", "any", "auto", 10},
        {"fmu/out/vehicle_status", "", "", 5.0, 0.0, true, false, "real_time", "drop", true, "reliable", "any", "auto", 10},
        {"fmu/out/vehicle_local_position", "", "", 20.0, 0.0, true, true, "data_high", "drop", true, "best_effort", "any", "auto", 10},
        {"Odometry", "", "", 10.0, 0.0, true, true, "data", "drop", false, "best_effort", "any", "auto", 10},
        {"cloud_registered", "", "", 2.0, 0.0, false, true, "data_low", "drop", false, "best_effort", "any", "auto", 5},
    };
    rs.ingress = {
        {"*/fmu/out/vehicle_odometry", "", true, 30.0, true, "any", "auto", 10},
        {"*/fmu/out/vehicle_status", "", true, 5.0, true, "any", "auto", 10},
        {"*/Odometry", "", true, 10.0, true, "any", "auto", 10},
    };
    return rs;
  }

  if (preset_name == "low_bandwidth") {
    rs.egress = {
        {"fmu/out/vehicle_odometry", "", "", 10.0, 0.0, true, true, "data_high", "drop", true, "best_effort", "any", "auto", 10},
        {"mavros/local_position/pose", "", "", 10.0, 0.0, true, false, "data_high", "drop", true, "best_effort", "any", "auto", 10},
        {"mavros/state", "", "", 5.0, 0.0, true, false, "real_time", "drop", true, "reliable", "any", "auto", 10},
        {"camera/image_raw/compressed", "", "", 2.0, 200.0, false, true, "background", "drop", false, "best_effort", "any", "best_effort", 5},
        {"cloud_registered", "", "", 1.0, 300.0, false, true, "background", "drop", false, "best_effort", "any", "best_effort", 5},
    };
    rs.ingress = {
        {"*/fmu/out/vehicle_odometry", "", true, 10.0, true, "any", "auto", 10},
        {"*/mavros/local_position/pose", "", true, 10.0, true, "any", "auto", 10},
        {"*/mavros/state", "", true, 5.0, true, "any", "auto", 10},
    };
    return rs;
  }

  if (preset_name == "vision_debug") {
    rs.egress = {
        {"fmu/out/vehicle_odometry", "", "", 30.0, 0.0, true, true, "data_high", "drop", true, "best_effort", "any", "auto", 10},
        {"Odometry", "", "", 15.0, 0.0, true, true, "data", "drop", false, "best_effort", "any", "auto", 10},
        {"mavros/local_position/pose", "", "", 30.0, 0.0, true, false, "data_high", "drop", true, "best_effort", "any", "auto", 10},
        {"camera/image_raw/compressed", "", "", 5.0, 0.0, true, true, "data_low", "drop", false, "best_effort", "any", "best_effort", 5},
        {"cloud_registered", "", "", 2.0, 0.0, true, true, "data_low", "drop", false, "best_effort", "any", "best_effort", 5},
    };
    rs.ingress = {
        {"*/fmu/out/vehicle_odometry", "", true, 30.0, true, "any", "auto", 10},
        {"*/Odometry", "", true, 15.0, true, "any", "auto", 10},
        {"*/mavros/local_position/pose", "", true, 30.0, true, "any", "auto", 10},
        {"*/camera/image_raw/compressed", "", true, 5.0, false, "any", "best_effort", 5},
    };
    return rs;
  }

  // Default: "formation"
  rs.egress = {
      {"fmu/out/vehicle_odometry", "", "", 30.0, 0.0, true, true, "data_high", "drop", true, "best_effort", "any", "auto", 10},
      {"mavros/local_position/pose", "", "", 30.0, 0.0, true, false, "data_high", "drop", true, "best_effort", "any", "auto", 10},
      {"mavros/local_position/velocity_local", "", "", 30.0, 0.0, true, false, "data_high", "drop", true, "best_effort", "any", "auto", 10},
      {"mavros/state", "", "", 10.0, 0.0, true, false, "real_time", "drop", true, "reliable", "any", "auto", 10},
      {"Odometry", "", "", 10.0, 0.0, true, true, "data", "drop", false, "best_effort", "any", "auto", 10},
      {"camera/image_raw/compressed", "", "", 5.0, 0.0, false, true, "data_low", "drop", false, "best_effort", "any", "best_effort", 5},
      {"cloud_registered", "", "", 2.0, 0.0, false, true, "data_low", "drop", false, "best_effort", "any", "best_effort", 5},
  };
  rs.ingress = {
      {"*/fmu/out/vehicle_odometry", "", true, 30.0, true, "any", "auto", 10},
      {"*/mavros/local_position/pose", "", true, 30.0, true, "any", "auto", 10},
      {"*/mavros/local_position/velocity_local", "", true, 30.0, true, "any", "auto", 10},
      {"*/mavros/state", "", true, 10.0, true, "any", "auto", 10},
  };
  return rs;
}

}  // namespace zenoh_bridge

#endif  // ZENOH_BRIDGE_PROTOCOL_HPP
