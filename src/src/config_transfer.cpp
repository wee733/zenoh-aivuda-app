#include "zenoh_bridge/bridge_node.hpp"
#include "zenoh_bridge/config_transaction.hpp"

#include <ctime>
#include <limits>
#include <rclcpp/expand_topic_or_service_name.hpp>

namespace zenoh_bridge {
namespace {
using json = nlohmann::json;

void require(bool valid, const std::string &message) {
  if (!valid) throw std::invalid_argument(message);
}

HttpResponse result(int code, json body) {
  HttpResponse response;
  response.status = code;
  response.body = body.dump();
  return response;
}

json failure(const std::string &message) { return {{"ok", false}, {"message", message}}; }

void checkFields(const json &value, const json &shape, const std::string &name) {
  require(value.is_object(), name + " must be an object");
  for (const auto &[key, item] : value.items()) {
    const auto spec = shape.find(key);
    const auto field = name + "." + key;
    require(spec != shape.end(), "Unknown field: " + field);
    if (spec->is_number_integer()) {
      require(item.is_number_integer() && item >= std::numeric_limits<int>::min() &&
              item <= std::numeric_limits<int>::max(), field + " must be an integer in range");
    } else if (spec->is_number_float()) {
      require(item.is_number(), field + " must be a number");
    } else {
      require(item.type() == spec->type(), "Invalid type for " + field);
    }
    if (spec->is_string()) require(item.get_ref<const std::string &>().find('\0') == std::string::npos,
                                   field + " contains a null character");
  }
}

void checkChoice(const json &value, const char *key, std::initializer_list<const char *> choices) {
  if (!value.contains(key)) return;
  require(std::any_of(choices.begin(), choices.end(), [&](const char *s) { return value.at(key) == s; }),
          std::string("Invalid ") + key + ": " + value.at(key).dump());
}

json portableSettings(const BridgeSettings &settings) {
  json fields = settings;
  for (const char *key : {"agent_id", "dds_shield_enabled", "dds_shield_scope", "dds_shield_domains"})
    fields.erase(key);
  return fields;
}

json portableRoutes(const RuleSet &rules) {
  json routes = {{"egress", json::array()}, {"ingress", json::array()}};
  for (const auto &rule : rules.egress) routes["egress"].push_back(portableRule(rule));
  for (const auto &rule : rules.ingress) routes["ingress"].push_back(portableRule(rule));
  return routes;
}

json makeExport(const std::string &agent, const std::string &robot_type, const RuleSet &rules,
                const BridgeSettings &settings, const SessionConfig &session) {
  std::time_t now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  char timestamp[21];
  std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return {{"format_version", 1}, {"exported_at", timestamp}, {"agent_id", agent}, {"robot_type", robot_type},
          {"routes", portableRoutes(rules)}, {"bridge_settings", portableSettings(settings)}, {"session_override", session}};
}

void checkConcreteKey(const std::string &key, bool single = false) {
  require(!key.empty() && key.find_first_of("*$") == std::string::npos &&
          (!single || key.find('/') == std::string::npos), "Expected a concrete Zenoh key: " + key);
  (void)zenoh::KeyExpr(key, false);
}

// Use the same identity and defaults as the runtime. Ignore measured counters
// when comparing two rules, so an idle export and a busy export diff equally.
json readRule(const json &item, bool egress, const std::string &agent, const std::string &domain) {
  const auto shape = egress ? portableRule(EgressRule{}) : portableRule(IngressRule{});
  checkFields(item, shape, egress ? "egress" : "ingress");
  checkChoice(item, "locality", {"any", "remote", "session_local", "local", "session_remote"});
  checkChoice(item, "qos", {"auto", "best_effort", "reliable", "sensor_data", "transient_local"});
  if (item.contains("history_depth")) require(item.at("history_depth") >= 1 && item.at("history_depth") <= 1000,
                                              "history_depth must be in [1,1000]");
  if (egress) {
    checkChoice(item, "priority", {"real_time", "realtime", "interactive_high", "interactive_low",
                                   "data_high", "high", "data", "normal", "data_low", "low", "background"});
    checkChoice(item, "congestion", {"drop", "block", "block_first"});
    checkChoice(item, "reliability", {"reliable", "best_effort"});
    auto rule = item.get<EgressRule>();
    rule.topic = normalizeRelativeTopic(rule.topic, agent);
    require(!rule.topic.empty(), "egress.topic is required");
    rclcpp::expand_topic_or_service_name("/" + rule.topic, "zenoh_bridge", "/");
    checkConcreteKey(makeDataKey(domain, agent, rule.topic, rule.zenoh_key));
    return portableRule(rule);
  }
  auto rule = item.get<IngressRule>();
  rule.pattern = trimSlashes(rule.pattern);
  require(!rule.pattern.empty(), "ingress.pattern is required");
  (void)zenoh::KeyExpr(makeDataSubKey(domain, rule.pattern), false);
  if (!rule.remap_prefix.empty()) {
    rclcpp::expand_topic_or_service_name("/" + trimSlashes(rule.remap_prefix) + "/sample", "zenoh_bridge", "/");
  }
  return portableRule(rule);
}

json planRoutes(const json &before, const json &incoming, bool merge, const std::string &agent,
                const std::string &domain, json &summary) {
  checkFields(incoming, before, "routes");
  auto after = before;
  int added = 0, modified = 0, deleted = 0;
  for (const char *direction : {"egress", "ingress"}) {
    const auto &existing = before.at(direction);
    const auto &imported = incoming.at(direction);
    const bool egress = std::string(direction) == "egress";
    const char *key = egress ? "topic" : "pattern";
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < existing.size(); ++i) index[existing.at(i).at(key).get<std::string>()] = i;
    std::set<std::string> seen;
    auto &rules = after.at(direction);
    if (!merge) rules.clear();
    for (const auto &item : imported) {
      auto rule = readRule(item, egress, agent, domain);
      const auto id = rule.at(key).get<std::string>();
      require(seen.insert(id).second, std::string("Duplicate ") + key + ": " + id);
      const auto found = index.find(id);
      if (found == index.end()) ++added;
      else if (existing.at(found->second) != rule) ++modified;
      if (merge && found != index.end()) rules[found->second] = rule;
      else rules.push_back(rule);
    }
    if (!merge) for (const auto &old : existing) if (!seen.count(old.at(key).get<std::string>())) ++deleted;
  }
  summary.update(json{{"added", added}, {"modified", modified}, {"deleted", deleted}});
  return after;
}

RuleSet asRules(const json &routes) {
  return {routes.at("egress").get<std::vector<EgressRule>>(), routes.at("ingress").get<std::vector<IngressRule>>()};
}

}  // namespace

HttpResponse ZenohBridgeNode::handleConfigTransfer(const HttpRequest &req, bool importing) {
  try {
    const std::string target = req.query_params.count("agent_id") ? req.query_params.at("agent_id") : "";
    if (!target.empty()) checkConcreteKey(target, true);
    std::unique_lock<std::mutex> writer(reconfigure_mutex_);
    const auto current = configSnapshot();
    if (!target.empty() && target != current->agent_id) {
      if (importing) return result(403, failure("Open the target node's local management page to import"));
      writer.unlock();
      auto remote = queryRemoteAgent(target, {{"action", "get"}});
      if (!remote.value("ok", false)) return result(503, remote);
      auto status = remote.at("status").get<AgentStatus>();
      require(status.agent_id == target, "Export source identity mismatch");
      return result(200, makeExport(target, status.robot_type, {status.egress, status.ingress},
                                    remote.at("bridge_settings").get<BridgeSettings>(),
                                    remote.at("session_config").get<SessionConfig>()));
    }
    auto old_rules = getCurrentRules();
    if (!importing)
      return result(200, makeExport(current->agent_id, robot_type_, old_rules, current->bridge, current->session));

    const auto body = json::parse(req.body);
    checkFields(body, {{"config", json::object()}, {"mode", ""}, {"dry_run", false},
                       {"include_agent_id", false}, {"sections", json::array()}}, "request");
    const auto &doc = body.at("config");
    checkFields(doc, {{"format_version", 1}, {"agent_id", ""}, {"robot_type", ""}, {"exported_at", ""},
                      {"routes", json::object()}, {"bridge_settings", json::object()},
                      {"session_override", json::object()}}, "config");
    require(doc.at("format_version") == 1, "Expected format_version 1");
    checkChoice(body, "mode", {"merge", "replace"});
    const bool merge = body.value("mode", "replace") == "merge";
    const bool dry = body.value("dry_run", false);
    const bool rename = body.value("include_agent_id", false);
    std::set<std::string> sections;
    for (const auto &section : body.value("sections", json{"routes", "bridge_settings", "session_override"})) {
      require(section == "routes" || section == "bridge_settings" || section == "session_override", "Unknown import section");
      require(sections.insert(section.get<std::string>()).second, "Duplicate import section");
    }
    require(!sections.empty() || rename, "Select at least one import section");
    auto next = *current;
    if (rename) {
      next.agent_id = doc.at("agent_id").get<std::string>();
      checkConcreteKey(next.agent_id, true);
      next.bridge.agent_id = next.agent_id == default_agent_id_ ? "" : next.agent_id;
    }

    if (sections.count("bridge_settings")) {
      const auto &patch = doc.at("bridge_settings");
      auto fields = portableSettings(merge ? next.bridge : BridgeSettings{});
      checkFields(patch, fields, "bridge_settings");
      fields.update(patch);
      checkConcreteKey(fields.at("domain_prefix").get<std::string>());
      checkChoice(fields, "default_preset", {"empty", "none", "formation", "uxrce_swarm", "vision_debug", "low_bandwidth"});
      next.bridge = mergedBridgeSettings(next.bridge, fields);
    }
    if (sections.count("session_override")) {
      const auto &patch = doc.at("session_override");
      json fields = merge ? current->session : SessionConfig{};
      checkFields(patch, fields, "session_override");
      for (const auto &[key, value] : patch.items()) {
        if (value.is_number_integer()) require(value >= 0, key + " must be nonnegative");
        if (value.is_array()) {
          for (const auto &entry : value) require(entry.is_string() && !entry.get_ref<const std::string &>().empty(),
                                                 key + " must contain endpoint strings");
        }
      }
      fields.update(patch);
      checkChoice(fields, "config_mode", {"structured", "raw_json5"});
      checkChoice(fields, "mode", {"peer", "client", "router"});
      checkChoice(fields, "shm_mode", {"lazy", "init"});
      checkChoice(fields, "acl_default_permission", {"deny", "allow"});
      checkChoice(fields, "peer_routing_mode", {"peer_to_peer", "linkstate"});
      next.session = fields.get<SessionConfig>();
      std::string preview;
      buildZenohConfig(next.session, preview);
    }

    json summary{{"added", 0}, {"modified", 0}, {"deleted", 0}};
    const auto before = portableRoutes(old_rules);
    const auto routes = sections.count("routes")
        ? planRoutes(before, doc.at("routes"), merge, next.agent_id, next.bridge.domain_prefix, summary) : before;
    const auto new_rules = asRules(routes);
    const bool session_changed = json(next.session) != json(current->session);
    const bool settings_changed = portableSettings(next.bridge) != portableSettings(current->bridge);
    const bool identity_changed = next.agent_id != current->agent_id;
    summary.update(json{{"egress", new_rules.egress.size()}, {"ingress", new_rules.ingress.size()},
                        {"bridge_settings_changed", settings_changed}, {"session_override_changed", session_changed},
                        {"agent_id_changed", identity_changed}});
    json response{{"ok", true}, {"dry_run", dry}, {"summary", summary}, {"agent_id", next.agent_id}};
    if (dry) return result(200, response);

    const ConfigFiles paths{{"bridge_settings", bridge_settings_file_}, {"routes", rules_file_},
                            {"session_override", session_override_file_}};
    try {
      ConfigFiles contents;
      if (sections.count("routes")) contents["routes"] = rulesToYaml(new_rules);
      if (sections.count("session_override")) contents["session_override"] = json(next.session).dump(2) + "\n";
      if (sections.count("bridge_settings") || rename) {
        // Preserve exactly what is durable for Shield and any unselected identity.
        auto disk = readConfigObject(bridge_settings_file_, true);
        (void)disk.get<BridgeSettings>();
        if (sections.count("bridge_settings")) disk.update(portableSettings(next.bridge));
        if (rename) disk["agent_id"] = next.bridge.agent_id;
        contents["bridge_settings"] = disk.dump(2) + "\n";
      }

      const bool reconnect = session_changed || identity_changed || next.bridge.domain_prefix != current->bridge.domain_prefix ||
                             next.bridge.liveliness_enabled != current->bridge.liveliness_enabled;
      const bool rebind = routes != before || next.bridge.allow_unprefixed_local != current->bridge.allow_unprefixed_local;
      auto activate = [&](const RuntimeConfig &cfg, const RuleSet &rules) {
        std::string message;
        if (reconnect) {
          if (!reconnectSessionLocked(cfg, false, message, &rules)) throw std::runtime_error(message);
        } else {
          publishConfig(cfg);
          if (rebind && !applyRulesLocked(rules, false, message)) throw std::runtime_error(message);
        }
      };
      bool activated = false;
      try {
        activate(next, new_rules);
        activated = true;
        commitConfigFiles(paths, contents);
      } catch (const std::exception &error) {
        // reconnectSessionLocked already restores its original generation on open failure.
        if (!reconnect || activated) {
          try { activate(*current, old_rules); }
          catch (const std::exception &rollback) {
            throw std::runtime_error(std::string(error.what()) + "; runtime rollback failed: " + rollback.what());
          }
        }
        throw;
      }
      if (identity_changed) {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        remote_agents_.erase(current->agent_id);
        remote_last_seen_ms_.erase(current->agent_id);
      }
      addLog("INFO", "Imported bridge configuration for " + next.agent_id);
      return result(200, response);
    } catch (const std::exception &e) {
      return result(500, failure(e.what()));
    }
  } catch (const std::exception &e) {
    return result(400, failure(e.what()));
  }
}
}  // namespace zenoh_bridge
