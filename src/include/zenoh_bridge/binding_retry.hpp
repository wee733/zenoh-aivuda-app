#pragma once

#include <chrono>
#include <string>

namespace zenoh_bridge {

// Owned by one route (one local topic for a wildcard ingress), under its binding lock.
struct BindingRetry {
  using Clock = std::chrono::steady_clock;
  std::string target, error;
  Clock::time_point retry_at{};
  unsigned step{0};

  bool ready(const std::string &candidate, Clock::time_point now = Clock::now()) const {
    return candidate != target || now >= retry_at;
  }

  // ament may include its prefix search path only on the first failed lookup.
  static bool missingType(const std::string &reason) {
    const auto package = reason.find("package '");
    return (package != std::string::npos && reason.find("' not found", package) != std::string::npos) ||
           reason.find("Unknown message type") != std::string::npos ||
           reason.find("unknown message type") != std::string::npos;
  }

  // A different target or failure starts a new incident and should be logged once.
  bool failed(const std::string &candidate, const std::string &reason, Clock::time_point now = Clock::now()) {
    const bool first = !step || target != candidate ||
        (error != reason && !(missingType(error) && missingType(reason)));
    if (first) {
      error = reason;
      step = 0;
    }
    constexpr int delays[] = {5, 15, 30};
    target = candidate;
    retry_at = now + std::chrono::seconds(delays[step]);
    if (step < 2) ++step;
    return first;
  }
};

}  // namespace zenoh_bridge
