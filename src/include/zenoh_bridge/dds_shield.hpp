#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace zenoh_bridge {

// Shared by the pre-ROS bootstrap and live settings transactions. Only the
// installed helper is trusted; a zero exit alone does not certify enforcement.
inline nlohmann::json invokeDdsShield(const std::string &script, const std::string &action,
                                      const std::string &scope, const std::string &domains,
                                      const std::string &owner) {
  if (!std::filesystem::path(owner).is_absolute() || !std::filesystem::path(script).is_absolute()) {
    throw std::invalid_argument("DDS Shield requires absolute helper and owner paths");
  }
  for (auto arg : {&script, &action, &scope, &domains, &owner})
    if (arg->find('\0') != std::string::npos) throw std::invalid_argument("NUL in DDS Shield argument");
  const char *args[] = {"/bin/bash", "--", script.c_str(), action.c_str(), scope.c_str(),
                        domains.c_str(), owner.c_str(), nullptr};
  std::string output;
  int fd[2];
  if (::pipe2(fd, O_CLOEXEC) != 0) throw std::runtime_error("Cannot open DDS Shield helper pipe");
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(fd[0]);
    if (::setpgid(0, 0) < 0) ::_exit(126);
    if (fd[1] == STDOUT_FILENO) {
      if (::fcntl(fd[1], F_SETFD, 0) < 0) ::_exit(126);
    } else {
      if (::dup2(fd[1], STDOUT_FILENO) < 0) ::_exit(126);
      ::close(fd[1]);
    }
    ::execv(args[0], const_cast<char *const *>(args));
    ::_exit(127);
  }
  ::close(fd[1]);
  if (pid < 0) {
    ::close(fd[0]);
    throw std::runtime_error("Cannot start DDS Shield helper");
  }
  // Only read after poll; keep the same deadline even if the child closes stdout
  // and hangs, keeps writing, or exits while a descendant holds its pipe open.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  int rc;
  try {
    bool eof = false;
    for (;;) {
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (ms <= 0) throw std::runtime_error("DDS Shield helper timed out");
      if (eof) {
        const pid_t done = ::waitpid(pid, &rc, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) throw std::runtime_error("Cannot wait for DDS Shield helper");
        if (ms > 10) ms = 10;
      }
      pollfd pfd{eof ? -1 : fd[0], POLLIN, 0};
      const int ready = ::poll(&pfd, 1, static_cast<int>(ms));
      if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
      if (ready < 0) throw std::runtime_error("Cannot poll DDS Shield helper");
      char buf[4096];
      const auto n = ::read(fd[0], buf, sizeof(buf));
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) throw std::runtime_error("Cannot read DDS Shield helper response");
      if (output.size() + n > 64 * 1024) throw std::runtime_error("DDS Shield helper response too large");
      output.append(buf, n);
      eof = n == 0;
    }
  } catch (...) {
    ::kill(-pid, SIGKILL);  // Include a stalled helper's children.
    ::kill(pid, SIGKILL);   // Also covers failure before the child sets its group.
    while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    ::close(fd[0]);
    throw;
  }
  ::close(fd[0]);
  const auto status = nlohmann::json::parse(output);
  const bool ok = status.at("ok").get<bool>();
  const bool active = status.at("active").get<bool>();
  for (const char *key : {"rx_dropped_packets", "tx_dropped_packets", "rx_dropped_bytes", "tx_dropped_bytes"}) {
    if (status.contains(key) && !status.at(key).is_number_unsigned())
      throw std::runtime_error(std::string("Invalid DDS Shield helper counter: ") + key);
  }
  if (status.contains("message") && !status.at("message").is_string())
    throw std::runtime_error("Invalid DDS Shield helper message");
  if (rc != 0 || !ok || (action == "enable" && !active) || (action == "disable" && active)) {
    throw std::runtime_error("DDS Shield " + action + " failed: " + status.value("message", "unexpected helper result"));
  }
  return status;
}

}  // namespace zenoh_bridge
