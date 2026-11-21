#ifndef ZENOH_BRIDGE_HTTP_SERVER_HPP
#define ZENOH_BRIDGE_HTTP_SERVER_HPP

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zenoh_bridge {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string query_string;
  std::map<std::string, std::string> query_params;
  std::map<std::string, std::string> headers;
  std::string body;
};

struct HttpResponse {
  int status{200};
  std::string content_type{"application/json; charset=utf-8"};
  std::string body{"{}"};
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

    running_.store(true);
    accept_thread_ = std::thread([this]() { acceptLoop(); });
    return true;
  }

  void stop() {
    if (!running_.exchange(false)) {
      return;
    }
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }
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
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
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
      std::thread([this, fd]() { handleClient(fd); }).detach();
    }
  }

  void handleClient(int fd) {
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
              if (k == "content-length") {
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

    if (header_end == std::string::npos) {
      ::close(fd);
      return;
    }

    HttpRequest req;
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
        req.query_string = target.substr(qpos + 1);
        parseQuery(req.query_string, req.query_params);
      } else {
        req.path = target;
      }
    }
    std::string hline;
    while (std::getline(hs, hline)) {
      if (!hline.empty() && hline.back() == '\r') hline.pop_back();
      const size_t colon = hline.find(':');
      if (colon != std::string::npos) {
        std::string k = hline.substr(0, colon);
        std::string v = hline.substr(colon + 1);
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        req.headers[k] = v;
      }
    }
    req.body = raw.substr(header_end + 4, content_length);

    HttpResponse resp;
    if (req.method == "OPTIONS") {
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
    out << "Content-Length: " << resp.body.size() << "\r\n";
    out << "Cache-Control: no-store\r\n";
    out << "Access-Control-Allow-Origin: *\r\n";
    out << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
    out << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    out << "Connection: close\r\n\r\n";
    out << resp.body;

    const std::string wire = out.str();
    size_t sent = 0;
    while (sent < wire.size()) {
      ssize_t n = ::send(fd, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) break;
      sent += static_cast<size_t>(n);
    }
    ::close(fd);
  }

  int listen_fd_{-1};
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  Handler handler_;
};

}  // namespace zenoh_bridge

#endif  // ZENOH_BRIDGE_HTTP_SERVER_HPP
