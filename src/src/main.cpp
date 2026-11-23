#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <system_error>
#include <vector>

#include <rcl/domain_id.h>
#include <rcl/remap.h>
#include <rcl_yaml_param_parser/parser.h>
#include <rclcpp/parameter_map.hpp>
#include <rclcpp/rclcpp.hpp>

#include "zenoh_bridge/bridge_node.hpp"
#include "zenoh_bridge/config_transaction.hpp"
#include "zenoh_bridge/dds_shield.hpp"

namespace {
namespace fs = std::filesystem;
using zenoh_bridge::readConfigObject;
using zenoh_bridge::StartupConfig;

// The lock inode is stable across atomic settings-file replacements. Do not
// unlink it on release. Hold it until the node, executor and ROS context stop.
class OwnerLock {
public:
  explicit OwnerLock(const fs::path &owner) {
    const auto path = owner.string() + ".lock";
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) throw std::system_error(errno, std::generic_category(), path);
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      const int e = errno;
      ::close(fd_);
      throw std::system_error(e, std::generic_category(), "Cannot exclusively own " + path);
    }
  }
  ~OwnerLock() { ::close(fd_); }
  OwnerLock(const OwnerLock &) = delete;
  OwnerLock &operator=(const OwnerLock &) = delete;
private:
  int fd_;
};

// Parse exactly the node-local argument set used below, without creating an
// rcl context or node. This handles --params-file, wildcard overrides and the
// remapped fully-qualified node name, not just literal '-p name:=...' tokens.
std::map<std::string, rclcpp::Parameter> overrides(const rclcpp::NodeOptions &options) {
  const auto *args = &options.get_rcl_node_options()->arguments;
  const auto alloc = rcl_get_default_allocator();
  auto remap = [&](auto fn, const std::string &name, const std::string &fallback) {
    char *raw = nullptr;
    const auto rc = fn(args, nullptr, name.c_str(), alloc, &raw);
    auto free = [&](char *p) { alloc.deallocate(p, alloc.state); };
    std::unique_ptr<char, decltype(free)> guard(raw, free);
    if (rc != RCL_RET_OK) throw std::runtime_error("Invalid ROS name remapping");
    return raw ? std::string(raw) : fallback;
  };
  auto name = remap(rcl_remap_node_name, "zenoh_bridge", "zenoh_bridge");
  auto ns = remap(rcl_remap_node_namespace, name, "/");
  const auto fqn = (ns == "/" ? "" : ns) + "/" + name;
  rcl_params_t *raw = nullptr;
  if (rcl_arguments_get_param_overrides(args, &raw) != RCL_RET_OK)
    throw std::runtime_error("Cannot read ROS parameter overrides");
  std::unique_ptr<rcl_params_t, decltype(&rcl_yaml_node_struct_fini)> guard(raw, rcl_yaml_node_struct_fini);
  std::map<std::string, rclcpp::Parameter> result;
  if (raw) {
    for (const auto &entry : rclcpp::parameter_map_from(raw, fqn.c_str()))
      for (const auto &p : entry.second) result.insert_or_assign(p.get_name(), p);
  }
  return result;
}

int run(int argc, char **argv) {
  rclcpp::NodeOptions options;
  options.arguments(std::vector<std::string>(argv + 1, argv + argc)).use_global_arguments(false);
  const auto params = overrides(options);
  const auto root = fs::canonical("/proc/self/exe").parent_path().parent_path();
  const char *data_env = std::getenv("AIVUDA_APP_RUNTIME_DATA_PATH");
  const fs::path data = data_env && *data_env ? fs::absolute(data_env) : root / "runtime";
  const auto runtime_it = params.find("runtime_config_file");
  StartupConfig startup;
  startup.runtime_path = runtime_it == params.end() ? (data / "runtime_config.json").string() : runtime_it->second.as_string();
  startup.runtime = readConfigObject(startup.runtime_path, runtime_it == params.end());
  const auto &j = startup.runtime;
  const auto owner_it = params.find("bridge_settings_file");
  const auto owner = j.value("bridge_settings_file", owner_it == params.end() ?
                             (data / "bridge_settings.json").string() : owner_it->second.as_string());
  if (owner.empty() || owner.find('\0') != std::string::npos) throw std::runtime_error("Invalid bridge_settings_file");
  if (owner_it != params.end() && j.contains("bridge_settings_file") &&
      fs::absolute(owner_it->second.as_string()).lexically_normal() != fs::absolute(owner).lexically_normal())
    throw std::runtime_error("Conflicting ROS and runtime bridge_settings_file owners");
  const auto path = fs::absolute(owner);
  fs::create_directories(path.parent_path());
  startup.settings_path = (fs::canonical(path.parent_path()) / path.filename()).string();
  OwnerLock lock(startup.settings_path);
  auto configuredPath = [&](const std::string &key) {
    const auto it = params.find(key);
    return j.value(key, it == params.end() ? std::string{} : it->second.as_string());
  };
  zenoh_bridge::recoverConfigFiles({{"bridge_settings", startup.settings_path},
                                    {"routes", configuredPath("rules_file")},
                                    {"session_override", configuredPath("session_override_file")}});
  startup.persisted = readConfigObject(startup.settings_path, true);
  startup.shield_script = (root / "scripts" / "dds_shield.sh").string();
  // Older runtime files include this redundant field. Never execute an
  // alternate, config-selected program to attest that the firewall is active.
  if (j.value("dds_shield_script", startup.shield_script) != startup.shield_script)
    throw std::runtime_error("dds_shield_script must be the installed DDS Shield helper");
  zenoh_bridge::BridgeSettings base;
  if (params.count("dds_shield_enabled")) base.dds_shield_enabled = params.at("dds_shield_enabled").as_bool();
  if (params.count("dds_shield_scope")) base.dds_shield_scope = params.at("dds_shield_scope").as_string();
  if (params.count("dds_shield_domains")) base.dds_shield_domains = params.at("dds_shield_domains").as_string();
  startup.shield_policy = mergedBridgeSettings(mergedBridgeSettings(base, j), startup.persisted);

  int64_t configured_domain = params.count("ros_domain_id") ? params.at("ros_domain_id").as_int() : -1;
  if (j.contains("ros_domain_id")) {
    if (!j.at("ros_domain_id").is_number_integer()) throw std::runtime_error("Invalid ros_domain_id");
    configured_domain = j.at("ros_domain_id").get<int64_t>();
  }
  if (configured_domain < -1 || configured_domain > 232) throw std::runtime_error("Invalid ros_domain_id");
  size_t domain = 0;
  if (configured_domain >= 0) domain = static_cast<size_t>(configured_domain);
  else if (rcl_get_default_domain_id(&domain) != RCL_RET_OK || domain > 232)
    throw std::runtime_error("Invalid ROS_DOMAIN_ID");
  startup.domain_id = static_cast<int>(domain);
  const auto &policy = startup.shield_policy;
  const std::string domains = policy.dds_shield_domains.empty() ? std::to_string(domain) : policy.dds_shield_domains;
  try {
    startup.shield_status = zenoh_bridge::invokeDdsShield(startup.shield_script,
        policy.dds_shield_enabled ? "enable" : "status", policy.dds_shield_scope, domains, startup.settings_path);
    if (!policy.dds_shield_enabled && startup.shield_status.at("active").get<bool>())
      startup.shield_error = "Owner protection remains active while configured disabled; explicitly disable to remove it";
  } catch (const std::exception &e) {
    if (policy.dds_shield_enabled) throw;
    startup.shield_error = e.what();  // Inspection is optional only when protection was not requested.
  }

  rclcpp::InitOptions init;
  init.set_domain_id(domain);
  rclcpp::init(argc, argv, init);
  try {
    auto node = std::make_shared<zenoh_bridge::ZenohBridgeNode>(startup, options);
    rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(), 4);
    executor.add_node(node);
    executor.spin();
  } catch (...) {
    rclcpp::shutdown();
    throw;
  }
  rclcpp::shutdown();
  return 0;
}
}  // namespace

int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "zenoh_bridge startup/runtime error: " << e.what() << '\n';
    return 1;
  }
}
