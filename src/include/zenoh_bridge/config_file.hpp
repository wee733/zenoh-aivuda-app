#pragma once

#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace zenoh_bridge {

inline nlohmann::json readConfigObject(const std::filesystem::path &path, bool optional) {
  namespace fs = std::filesystem;
  using json = nlohmann::json;
  // Distinguish a missing first-run file from EACCES, a broken symlink, a
  // directory/FIFO, partial JSON or an invalid root. None may mean disabled.
  const auto status = fs::symlink_status(path);
  if (optional && status.type() == fs::file_type::not_found) return json::object();
  if (!fs::is_regular_file(status)) throw std::runtime_error("Not a regular configuration file: " + path.string());
  try {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open");
    // Do not let the JSON parser's last-key-wins rule turn a conflicting
    // duplicate (particularly an enabled flag) into an apparently valid policy.
    std::vector<std::set<std::string>> keys;
    auto value = json::parse(in, [&](int, json::parse_event_t event, json &token) {
      if (event == json::parse_event_t::object_start) keys.emplace_back();
      if (event == json::parse_event_t::key && !keys.back().insert(token.get<std::string>()).second)
        throw std::runtime_error("duplicate JSON key: " + token.dump());
      if (event == json::parse_event_t::object_end) keys.pop_back();
      return true;
    });
    if (in.bad() || !value.is_object()) throw std::runtime_error("expected a readable JSON object");
    return value;
  } catch (const std::exception &e) {
    throw std::runtime_error(path.string() + ": " + e.what());
  }
}

}  // namespace zenoh_bridge
