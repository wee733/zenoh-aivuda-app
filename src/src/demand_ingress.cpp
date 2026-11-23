#include "zenoh_bridge/bridge_node.hpp"

namespace zenoh_bridge {
namespace {

// Mirror the sender's discovery preference: namespaced first, then bare, then
// lexicographically first matching type if several types share the ROS name.
const LocalTopicInfo *findPublisher(const AgentStatus &peer, const EgressRule &route) {
  const std::string bare = "/" + route.topic;
  const std::string prefixed = "/" + peer.agent_id + bare;
  const LocalTopicInfo *best = nullptr;
  for (const auto &topic : peer.local_topics) {
    if (topic.pub_count <= 0 || topic.type.empty() || (!route.type.empty() && route.type != topic.type)) continue;
    if (topic.name != prefixed && !(peer.allow_unprefixed_local && topic.name == bare)) continue;
    if (!best || (topic.name == prefixed && best->name != prefixed) ||
        (topic.name == best->name && topic.type < best->type)) best = &topic;
  }
  return best;
}

}  // namespace

// A silent encoder cannot supply a first frame for topic discovery. Advertise the
// peer's typed egress as a local ROS publisher, then use its reader count as demand.
void ZenohBridgeNode::refreshDemandIngress() {
  std::unique_lock<std::mutex> writer(reconfigure_mutex_, std::try_to_lock);
  if (!writer) return;
  std::vector<std::shared_ptr<ActiveIngress>> receivers;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto &ig : ingress_list_) {
      if (ig->rule.enabled && ig->rule.on_demand) receivers.push_back(ig);
    }
  }
  if (receivers.empty()) return;
  const auto config = configSnapshot();
  if (!config->active_session) return;
  struct Offer { std::string source, topic, key, type, qos; };
  std::vector<Offer> offers;
  {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    const auto now = nowEpochMs();
    for (const auto &[source, peer] : remote_agents_) {
      if (now - remote_last_seen_ms_.at(source) >= 5000 || peer.domain_prefix != config->bridge.domain_prefix) continue;
      for (const auto &route : peer.egress) {
        if (!route.enabled || route.topic.empty()) continue;
        const auto *publisher = findPublisher(peer, route);
        // An empty inventory means the camera is off. Old statuses may omit
        // the inventory altogether: retain their bound or explicitly typed route.
        if (peer.has_topic_inventory && !publisher) continue;
        const auto &type = publisher ? publisher->type :
            (route.resolved_type.empty() ? route.type : route.resolved_type);
        if (type.empty()) continue;
        const auto key = route.effective_key.empty() ?
            makeDataKey(peer.domain_prefix, source, route.topic, route.zenoh_key) : route.effective_key;
        const auto &qos = publisher && route.qos == "auto" ? publisher->reliability : route.qos;
        offers.push_back({source, route.topic, key, type, qos});
      }
    }
  }
  for (const auto &ig : receivers) {
    std::set<ActiveIngress::DemandBinding> wanted;
    std::set<std::string> advertised;
    for (const auto &offer : offers) {
      try {
        zenoh::KeyExpr pattern(ig->zenoh_pattern);
        if ((ig->rule.exclude_self && offer.source == ig->agent_id) ||
            (ig->rule.locality == "remote" && offer.source == ig->agent_id) ||
            (ig->rule.locality == "session_local" && offer.source != ig->agent_id)) continue;
        const zenoh::KeyExpr key(offer.key);
        if (!pattern.includes(key)) continue;
        const auto topic = localIngressTopic(ig->rule, offer.source, offer.topic);
        advertised.insert(topic);
        auto ep = getOrCreateIngressEndpoint(ig, topic, offer.type, offer.qos);
        if (!ep) continue;
        if (ep->ros_pub->get_subscription_count() == 0) continue;
        const ActiveIngress::DemandBinding binding{offer.key, offer.source, topic, offer.type};
        wanted.insert(binding);
        if (ig->demand_subs.count(binding)) continue;
        auto opts = zenoh::Session::SubscriberOptions::create_default();
        opts.allowed_origin = parseLocality(ig->rule.locality);
        std::weak_ptr<ActiveIngress> weak = ig;
        ig->demand_subs.emplace(binding, config->active_session->declare_subscriber(key,
            [this, weak, ep, source = offer.source](const zenoh::Sample &sample) {
              if (auto current = weak.lock()) handleZenohSample(current, sample, ep, source);
            }, zenoh::closures::none, std::move(opts)));
      } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(ig->endpoints_mutex);
        ig->last_error = offer.key + ": " + e.what();
      }
    }
    for (auto it = ig->demand_subs.begin(); it != ig->demand_subs.end();) {
      if (!wanted.count(it->first)) it = ig->demand_subs.erase(it);
      else ++it;
    }
    std::lock_guard<std::mutex> state(state_mutex_);
    std::lock_guard<std::mutex> endpoints(ig->endpoints_mutex);
    for (auto it = ig->binding_retries.begin(); it != ig->binding_retries.end();) {
      if (!advertised.count(it->first)) it = ig->binding_retries.erase(it);
      else ++it;
    }
    for (auto it = ig->endpoints.begin(); it != ig->endpoints.end();) {
      if (advertised.count(it->first)) { ++it; continue; }
      const std::string topic = it->first;
      it = ig->endpoints.erase(it);
      bool owned_elsewhere = false;
      for (const auto &other : ingress_list_) {
        if (other == ig) continue;
        std::lock_guard<std::mutex> lock(other->endpoints_mutex);
        owned_elsewhere |= other->endpoints.count(topic) != 0;
      }
      if (!owned_elsewhere) ingress_owned_topics_.erase(topic);
    }
  }
}

}  // namespace zenoh_bridge
