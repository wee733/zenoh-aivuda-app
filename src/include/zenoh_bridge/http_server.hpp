#ifndef ZENOH_BRIDGE_HTTP_SERVER_HPP
#define ZENOH_BRIDGE_HTTP_SERVER_HPP

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace zenoh_bridge {

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> query_params;
  std::string body;
  // These capabilities come only from the HTTP transport, never from JSON/RPC.
  bool shield_admin{false};
  bool shield_token_required{true};
  bool shield_management_available{false};
};

struct HttpResponse {
  int status{200};
  std::string content_type{"application/json; charset=utf-8"};
  std::string body{"{}"};
  using WriteChunk = std::function<bool(std::string_view)>;
  std::function<void(const WriteChunk &)> stream;
};

class MiniHttpServer {
public:
  using Handler = std::function<HttpResponse(const HttpRequest &)>;

  MiniHttpServer() = default;
  ~MiniHttpServer() { stop(); }

  MiniHttpServer(const MiniHttpServer &) = delete;
  MiniHttpServer &operator=(const MiniHttpServer &) = delete;

  bool start(const std::string &bind_host, int port, Handler handler) {
    handler_ = std::move(handler);
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return false;
    }

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind_host.empty() || bind_host == "0.0.0.0") {
      addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, bind_host.c_str(), &addr.sin_addr) <= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    if (::listen(listen_fd_, 32) < 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }

    loopback_binding_ = ntohl(addr.sin_addr.s_addr) == INADDR_LOOPBACK;
    const char *token = std::getenv("ZENOH_BRIDGE_SHIELD_TOKEN");
    shield_token_ = token ? token : "";
    running_.store(true);
    accept_thread_ = std::thread([this]() { acceptLoop(); });
    return true;
  }

  void stop() {
    if (!running_.exchange(false)) {
      return;
    }
    ::shutdown(listen_fd_, SHUT_RDWR);
    if (accept_thread_.joinable()) accept_thread_.join();
    ::close(listen_fd_);
    listen_fd_ = -1;
    // A policy writer may still be committing/rolling back. Do not destroy
    // its node or release the installation lock until the request finishes.
    std::unique_lock<std::mutex> lk(workers_mutex_);
    for (int fd : clients_) ::shutdown(fd, SHUT_RDWR);  // Wake workers still reading a request.
    workers_done_.wait(lk, [this] { return clients_.empty(); });
  }

  bool isRunning() const { return running_.load(); }

private:
  static std::string urlDecode(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '%' && i + 2 < in.size() &&
          std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
          std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
        const std::string hex = in.substr(i + 1, 2);
        out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
        i += 2;
      } else if (in[i] == '+') {
        out.push_back(' ');
      } else {
        out.push_back(in[i]);
      }
    }
    return out;
  }

  static void parseQuery(const std::string &qs, std::map<std::string, std::string> &out) {
    size_t start = 0;
    while (start < qs.size()) {
      size_t amp = qs.find('&', start);
      if (amp == std::string::npos) amp = qs.size();
      const std::string pair = qs.substr(start, amp - start);
      const size_t eq = pair.find('=');
      if (eq != std::string::npos) {
        out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
      } else if (!pair.empty()) {
        out[urlDecode(pair)] = "";
      }
      start = amp + 1;
    }
  }

  static std::string statusReason(int code) {
    switch (code) {
      case 200: return "OK";
      case 204: return "No Content";
      case 400: return "Bad Request";
      case 403: return "Forbidden";
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
      case 409: return "Conflict";
      case 500: return "Internal Server Error";
      case 503: return "Service Unavailable";
      default: return "OK";
    }
  }

  void acceptLoop() {
    while (running_.load()) {
      sockaddr_in client_addr{};
      socklen_t client_len = sizeof(client_addr);
      int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
      if (fd < 0) {
        if (!running_.load()) break;
        continue;
      }
      std::lock_guard<std::mutex> lk(workers_mutex_);
      clients_.insert(fd);
      try {
        const bool local_peer = (ntohl(client_addr.sin_addr.s_addr) >> 24) == 127;
        std::thread([this, fd, local_peer] {
          try { handleClient(fd, local_peer); } catch (...) { /* Drop the connection. */ }
          std::lock_guard<std::mutex> done(workers_mutex_);
          clients_.erase(fd);
          ::close(fd);
          if (clients_.empty()) workers_done_.notify_all();
        }).detach();
      } catch (const std::system_error &) {
        clients_.erase(fd);
        ::close(fd);
      }
    }
  }

  static bool isLocalAddress(const std::string &literal) {
    in_addr v4{};
    in6_addr v6{};
    const bool ipv4 = ::inet_pton(AF_INET, literal.c_str(), &v4) == 1;
    if (!ipv4 && ::inet_pton(AF_INET6, literal.c_str(), &v6) != 1) return false;
    if (ipv4 ? (ntohl(v4.s_addr) >> 24) == 127 : IN6_IS_ADDR_LOOPBACK(&v6)) return true;

    // Read current interface addresses so DHCP changes do not leave a stale grant.
    ifaddrs *interfaces{};
    if (::getifaddrs(&interfaces) != 0) return false;
    bool found = false;
    for (auto *it = interfaces; it && !found; it = it->ifa_next) {
      if (!it->ifa_addr || !(it->ifa_flags & IFF_UP)) continue;
      if (ipv4 && it->ifa_addr->sa_family == AF_INET) {
        found = reinterpret_cast<const sockaddr_in *>(it->ifa_addr)->sin_addr.s_addr == v4.s_addr;
      } else if (!ipv4 && it->ifa_addr->sa_family == AF_INET6) {
        found = IN6_ARE_ADDR_EQUAL(&reinterpret_cast<const sockaddr_in6 *>(it->ifa_addr)->sin6_addr, &v6);
      }
    }
    ::freeifaddrs(interfaces);
    return found;
  }

  static std::string normalizedHost(std::string name) {
    if (!name.empty() && name.back() == '.') name.pop_back();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
  }

  static bool localAuthority(const std::string &authority, bool from_gateway) {
    if (authority.empty()) return false;
    const bool bracketed = authority.front() == '[';
    const auto end = bracketed ? authority.find(']') : authority.find(':');
    if (bracketed && end == std::string::npos) return false;
    const auto host = normalizedHost(bracketed ? authority.substr(1, end - 1) : authority.substr(0, end));
    if (host == "localhost" || host == "127.0.0.1" || host == "::1") return true;
    if (!from_gateway) return false;
    if (isLocalAddress(host)) return true;
    char name[256]{};
    if (::gethostname(name, sizeof(name) - 1) == 0) {
      const auto system_name = normalizedHost(name);
      if (host == system_name || host == system_name + ".local") return true;
    }
    const char *configured = std::getenv("AIVUDA_OS_AVAHI_HOSTNAME");
    if (!configured || !*configured) return false;
    auto avahi = normalizedHost(configured);
    if (avahi.size() > 6 && avahi.compare(avahi.size() - 6, 6, ".local") == 0) avahi.resize(avahi.size() - 6);
    return host == avahi || host == avahi + ".local";
  }

  void handleClient(int fd, bool local_peer) {
    timeval tv{};
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string raw;
    raw.reserve(4096);
    char buf[4096];
    size_t header_end = std::string::npos;
    size_t content_length = 0;
    HttpRequest req;
    std::map<std::string, std::string> admin_headers;
    bool unique_admin_headers = true;

    while (raw.size() < 4 * 1024 * 1024) {
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) break;
      raw.append(buf, static_cast<size_t>(n));
      if (header_end == std::string::npos) {
        header_end = raw.find("\r\n\r\n");
        if (header_end != std::string::npos) {
          std::istringstream hs(raw.substr(0, header_end));
          std::string line;
          std::getline(hs, line);
          while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
              std::string k = line.substr(0, colon);
              std::transform(k.begin(), k.end(), k.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
              if (k == "host" || k == "origin" || k == "x-forwarded-for" ||
                  k == "x-forwarded-proto" || k == "x-forwarded-host" ||
                  k == "sec-fetch-site" || k == "x-dds-shield-token") {
                const size_t start = line.find_first_not_of(" \t", colon + 1);
                const size_t end = line.find_last_not_of(" \t");
                const auto value = start == std::string::npos ? "" : line.substr(start, end - start + 1);
                if (!admin_headers.emplace(k, value).second) unique_admin_headers = false;
              } else if (k == "content-length") {
                std::string v = line.substr(colon + 1);
                try {
                  content_length = static_cast<size_t>(std::stoull(v));
                } catch (...) {
                  content_length = 0;
                }
              }
            }
          }
        }
      }
      if (header_end != std::string::npos && raw.size() >= header_end + 4 + content_length) {
        break;
      }
    }

    // Do not dispatch a buffered request once stop() has started draining handlers.
    if (!isRunning() || header_end == std::string::npos || raw.size() - header_end - 4 < content_length) return;

    std::istringstream hs(raw.substr(0, header_end));
    std::string request_line;
    std::getline(hs, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();
    {
      std::istringstream rl(request_line);
      std::string target;
      rl >> req.method >> target;
      const size_t qpos = target.find('?');
      if (qpos != std::string::npos) {
        req.path = target.substr(0, qpos);
        parseQuery(target.substr(qpos + 1), req.query_params);
      } else {
        req.path = target;
      }
    }
    req.body = raw.substr(header_end + 4, content_length);

    const auto &host = admin_headers["host"];
    const auto origin = admin_headers.find("origin");
    const auto forwarded = admin_headers.find("x-forwarded-for");
    const auto proto = admin_headers.find("x-forwarded-proto");
    const auto authority = admin_headers.find("x-forwarded-host");
    const bool from_gateway = forwarded != admin_headers.end() && authority != admin_headers.end() &&
        authority->second == host && proto != admin_headers.end() &&
        (proto->second == "http" || proto->second == "https");
    // The package's Caddy proxy replaces untrusted forwarding headers. Trust an
    // interface address only behind that loopback hop, with one original client.
    const bool direct = forwarded == admin_headers.end() && authority == admin_headers.end() && proto == admin_headers.end();
    const bool local_client = loopback_binding_ && local_peer && localAuthority(host, from_gateway) &&
        (direct || (from_gateway && isLocalAddress(forwarded->second)));
    const bool same_origin = origin == admin_headers.end() || (from_gateway
        ? origin->second == proto->second + "://" + host
        : origin->second == "http://" + host);
    const auto site = admin_headers.find("sec-fetch-site");
    const bool same_site_request = site == admin_headers.end() || site->second == "same-origin" || site->second == "none";
    req.shield_token_required = !shield_token_.empty() || !loopback_binding_;
    const bool local_origin = unique_admin_headers && !host.empty() && same_origin && same_site_request;
    req.shield_management_available = local_origin && (!shield_token_.empty() || local_client);
    req.shield_admin = req.shield_management_available &&
        (!req.shield_token_required || admin_headers["x-dds-shield-token"] == shield_token_);

    const bool config_transfer = req.path == "/config/export" || req.path == "/api/config/export" ||
                                 req.path == "/config/import" || req.path == "/api/config/import";
    HttpResponse resp;
    if (config_transfer && !local_origin) {
      resp.status = 403;
      resp.body = R"({"ok":false,"message":"Configuration transfer requires a same-origin request"})";
    } else if (req.method == "OPTIONS") {
      resp.status = 204;
      resp.body.clear();
    } else if (handler_) {
      try {
        resp = handler_(req);
      } catch (const std::exception &e) {
        resp.status = 500;
        resp.body = std::string("{\"ok\":false,\"message\":\"") + e.what() + "\"}";
      }
    } else {
      resp.status = 503;
      resp.body = "{\"ok\":false,\"message\":\"No handler\"}";
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << resp.status << " " << statusReason(resp.status) << "\r\n";
    out << "Content-Type: " << resp.content_type << "\r\n";
    if (!resp.stream) out << "Content-Length: " << resp.body.size() << "\r\n";
    out << "Cache-Control: no-store\r\n";
    if (!resp.stream && !config_transfer) out << "Access-Control-Allow-Origin: *\r\n";
    out << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
    out << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    out << "Connection: close\r\n\r\n";
    if (!resp.stream) out << resp.body;

    const std::string wire = out.str();
    auto write = [fd](std::string_view bytes) {
      while (!bytes.empty()) {
        const ssize_t n = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (n <= 0) return false;
        bytes.remove_prefix(static_cast<size_t>(n));
      }
      return true;
    };
    if (write(wire) && resp.stream) resp.stream(write);
  }

  int listen_fd_{-1};
  bool loopback_binding_{false};
  std::string shield_token_;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  std::mutex workers_mutex_;
  std::condition_variable workers_done_;
  std::set<int> clients_;
  Handler handler_;
};

}  // namespace zenoh_bridge

#endif  // ZENOH_BRIDGE_HTTP_SERVER_HPP
