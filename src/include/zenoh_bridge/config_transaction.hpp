#pragma once

#include "zenoh_bridge/config_file.hpp"
#include "zenoh_bridge/protocol.hpp"

#include <map>

namespace zenoh_bridge {

// Switch the existing settings, session and routes files under their common
// owner lock. A journal surviving process/power loss rolls the whole group
// back before the next node reads any settings or opens DDS.
using ConfigFiles = std::map<std::string, std::string>;

inline void removeAndSync(const std::string &path) {
  if (!std::filesystem::remove(path)) return;
  auto parent = std::filesystem::absolute(path).parent_path();
  int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("Could not open directory " + parent.string());
  int rc;
  do { rc = ::fsync(fd); } while (rc < 0 && errno == EINTR);
  ::close(fd);
  if (rc < 0) throw std::runtime_error("Could not sync directory " + parent.string());
}

inline void recoverConfigFiles(const ConfigFiles &paths) {
  const auto journal = paths.at("bridge_settings") + ".import-journal";
  if (std::filesystem::symlink_status(journal).type() == std::filesystem::file_type::not_found) return;
  auto entries = readConfigObject(journal, false);
  for (const auto &[key, record] : entries.items()) {
    const auto &path = paths.at(key);
    if (path.empty() || record.at("path") != std::filesystem::absolute(path).lexically_normal().string())
      throw std::runtime_error("Import journal configuration path changed for " + key);
    if (!record.at("previous").is_null() && !record.at("previous").is_string())
      throw std::runtime_error("Invalid import journal for " + key);
  }
  for (const auto &[key, record] : entries.items()) {
    if (record.at("previous").is_null()) removeAndSync(paths.at(key));
    else atomicWriteFile(paths.at(key), record.at("previous").get<std::string>());
  }
  removeAndSync(journal);
}

inline void commitConfigFiles(const ConfigFiles &paths, const ConfigFiles &contents) {
  auto journal = paths.at("bridge_settings") + ".import-journal";
  if (std::filesystem::exists(journal)) throw std::runtime_error("Unresolved configuration import journal");
  nlohmann::json backups = nlohmann::json::object();
  std::set<std::filesystem::path> distinct{std::filesystem::weakly_canonical(journal)};
  for (const auto &[key, path] : paths) {
    if (!path.empty() && !distinct.insert(std::filesystem::weakly_canonical(path)).second)
      throw std::runtime_error("Conflicting file paths for " + key);
  }
  for (const auto &[key, content] : contents) {
    const auto &path = paths.at(key);
    if (path.empty()) throw std::runtime_error("No file configured for " + key);
    const auto type = std::filesystem::symlink_status(path).type();
    nlohmann::json previous = nullptr;
    if (type != std::filesystem::file_type::not_found) {
      if (type != std::filesystem::file_type::regular) throw std::runtime_error("Expected regular file: " + path);
      std::ifstream in(path, std::ios::binary);
      if (!in) throw std::runtime_error("Cannot read " + path);
      previous = std::string(std::istreambuf_iterator<char>(in), {});
      if (in.bad()) throw std::runtime_error("Error reading " + path);
      if (previous == content) continue;
    }
    backups[key] = {{"path", std::filesystem::absolute(path).lexically_normal().string()}, {"previous", previous}};
  }
  if (backups.empty()) return;
  const auto journal_contents = backups.dump() + "\n";
  bool started_writes = false;
  try {
    atomicWriteFile(journal, journal_contents);
    started_writes = true;
    for (const auto &[key, record] : backups.items()) atomicWriteFile(paths.at(key), contents.at(key));
    removeAndSync(journal);
  } catch (const std::exception &error) {
    try {
      // If unlink succeeded but its directory sync failed, the journal may
      // already be gone. Restore it before undoing any of the live files.
      if (started_writes && !std::filesystem::exists(journal)) atomicWriteFile(journal, journal_contents);
      recoverConfigFiles(paths);
    } catch (const std::exception &rollback) {
      throw std::runtime_error(std::string(error.what()) + "; file rollback failed: " + rollback.what());
    }
    throw;
  }
}
}  // namespace zenoh_bridge
