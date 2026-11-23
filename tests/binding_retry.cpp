#include "zenoh_bridge/binding_retry.hpp"
#include <cassert>

int main() {
  using namespace std::chrono_literals;
  using zenoh_bridge::BindingRetry;
  BindingRetry retry;
  auto now = BindingRetry::Clock::time_point{};
  const char *error = "package 'unavailable' not found";
  assert(retry.ready("topic", now));
  for (auto delay : {5s, 15s, 30s, 30s}) {
    assert(retry.failed("topic", error, now) == (delay == 5s));
    assert(!retry.ready("topic", now + delay - 1ms));
    now += delay;
    assert(retry.ready("topic", now));
  }
  assert(!retry.failed("topic", "Unknown message type", now));
  assert(!retry.failed("topic", "package 'unavailable' not found, searching: [/opt/ros]", now));
  assert(retry.error == error);
  assert(retry.failed("topic", "invalid QoS settings", now));
  assert(retry.ready("topic", now + 5s));
  assert(retry.ready("new type", now));
  retry = {};
  assert(retry.ready("topic", now));
  assert(retry.failed("topic", error, now));
}
