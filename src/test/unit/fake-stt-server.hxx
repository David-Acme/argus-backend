#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace
{

// Minimal in-process HTTP server standing in for the argus-stt wire in unit tests.
class FakeSttServer
{
public:
  explicit FakeSttServer(int status = 200)
      : status_(status)
  {
    listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listen_, 4);
    socklen_t len = sizeof(addr);
    ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { serve(); });
  }

  ~FakeSttServer()
  {
    stop();
  }

  int port() const { return port_; }

  std::map<std::string, int> requests() const
  {
    std::lock_guard lock(mutex_);
    return requests_;
  }

  size_t lastBodySize() const
  {
    std::lock_guard lock(mutex_);
    return lastBodySize_;
  }

  void stop()
  {
    if (listen_ < 0)
      return;
    ::close(listen_);
    listen_ = -1;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
    if (thread_.joinable())
      thread_.join();
  }

private:
  // Reads until the head terminator, then the Content-Length body if any.
  static std::string readRequest(int fd)
  {
    std::string data;
    char buffer[4096];
    const auto bodyStart = [&] {
      const auto split = data.find("\r\n\r\n");
      if (split == std::string::npos)
        return std::string::npos;
      const auto cl = data.find("Content-Length: ");
      if (cl == std::string::npos)
        return split + 4;
      const auto eol = data.find("\r\n", cl);
      return split + 4 +
             std::stoul(data.substr(cl + 16, eol - cl - 16));
    };
    while (data.find("\r\n\r\n") == std::string::npos) {
      const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
      if (n <= 0)
        break;
      data.append(buffer, static_cast<size_t>(n));
    }
    for (;;) {
      const auto need = bodyStart();
      if (need == std::string::npos || data.size() >= need)
        break;
      const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
      if (n <= 0)
        break;
      data.append(buffer, static_cast<size_t>(n));
    }
    return data;
  }

  void serve()
  {
    while (listen_ >= 0) {
      const int fd = ::accept(listen_, nullptr, nullptr);
      if (fd < 0)
        continue;
      const std::string request = readRequest(fd);
      const auto lineEnd = request.find("\r\n");
      const std::string line =
          lineEnd == std::string::npos ? request : request.substr(0, lineEnd);
      const auto space1 = line.find(' ');
      const auto space2 = line.find(' ', space1 + 1);
      const std::string method =
          space1 == std::string::npos ? "" : line.substr(0, space1);
      const std::string path =
          space2 == std::string::npos ? "" : line.substr(space1 + 1,
                                                        space2 - space1 - 1);
      const auto bodyStart = request.find("\r\n\r\n");

      {
        std::lock_guard lock(mutex_);
        requests_[method + " " + path]++;
        if (bodyStart != std::string::npos)
          lastBodySize_ = request.size() - bodyStart - 4;
      }

      std::string lang;
      const auto langPos = path.find("lang=");
      if (langPos != std::string::npos)
        lang = path.substr(langPos + 5);
      const std::string text =
          lang.empty() ? "hola default" : "hola " + lang;
      const std::string bodyJson =
          status_ == 200
              ? "{\"status\":200,\"info\":{\"text\":\"" + text +
                    "\"},\"errors\":null}"
              : "{\"status\":" + std::to_string(status_) +
                    ",\"info\":null,\"errors\":{\"code\":\"STT_NOT_LOADED\","
                    "\"message\":\"engine down\"}}";
      const std::string response =
          "HTTP/1.1 " + std::to_string(status_) +
          (status_ == 200 ? " OK" : " Unavailable") +
          "\r\nContent-Type: application/json\r\n"
          "Content-Length: " +
          std::to_string(bodyJson.size()) +
          "\r\nConnection: close\r\n\r\n" + bodyJson;

      size_t sent = 0;
      while (sent < response.size()) {
        const auto n = ::send(fd, response.data() + sent,
                              response.size() - sent, 0);
        if (n <= 0)
          break;
        sent += static_cast<size_t>(n);
      }
      ::close(fd);
    }
  }

  int listen_{-1};
  int port_{0};
  int status_{200};
  size_t lastBodySize_{0};
  std::map<std::string, int> requests_;
  mutable std::mutex mutex_;
  std::thread thread_;
};

} // namespace
