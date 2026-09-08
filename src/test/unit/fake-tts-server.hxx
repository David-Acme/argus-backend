#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
#include <vector>

namespace
{
constexpr const char* kConfigBody =
    R"({"status":200,"info":{"sampleRate":22050,"defaultSpeed":1.25},"errors":null})";

// Minimal in-process HTTP server standing in for the argus-tts wire in unit tests.
class FakeTtsServer
{
public:
  explicit FakeTtsServer(int status = 200)
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

  ~FakeTtsServer()
  {
    stop();
  }

  int port() const { return port_; }

  std::map<std::string, int> requests() const
  {
    std::lock_guard lock(mutex_);
    return requests_;
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

      {
        std::lock_guard lock(mutex_);
        requests_[method + " " + path]++;
      }

      std::string response;
      if (status_ != 200) {
        const std::string bodyJson =
            "{\"status\":" + std::to_string(status_) +
            ",\"info\":null,\"errors\":{\"code\":\"TTS_NOT_LOADED\","
            "\"message\":\"engine down\"}}";
        response = "HTTP/1.1 " + std::to_string(status_) +
                   " Unavailable\r\nContent-Type: application/json\r\n"
                   "Content-Length: " +
                   std::to_string(bodyJson.size()) +
                   "\r\nConnection: close\r\n\r\n" + bodyJson;
      }
      else if (path == "/tts/v1/config" && method == "GET") {
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                   "Content-Length: " +
                   std::to_string(std::strlen(kConfigBody)) +
                   "\r\nConnection: close\r\n\r\n" + kConfigBody;
      }
      else if (path == "/tts/v1/synthesize" && method == "POST") {
        std::string floats(8 * 4, '\0');
        for (size_t i = 0; i < 8; ++i) {
          const float sample = 0.25F;
          std::memcpy(floats.data() + i * 4, &sample, sizeof(sample));
        }
        response = "HTTP/1.1 200 OK\r\nContent-Type: audio/x-argus-pcm-f32\r\n"
                   "X-Argus-Sample-Rate: 22050\r\nContent-Length: " +
                   std::to_string(floats.size()) +
                   "\r\nConnection: close\r\n\r\n" + floats;
      }
      else if (path == "/tts/v1/synthesize-stream" && method == "POST") {
        std::string chunkData(64 * 4, '\0');
        for (size_t i = 0; i < 64; ++i) {
          const float sample = 0.25F;
          std::memcpy(chunkData.data() + i * 4, &sample, sizeof(sample));
        }
        response =
            "HTTP/1.1 200 OK\r\nContent-Type: audio/x-argus-pcm-f32\r\n"
            "X-Argus-Sample-Rate: 22050\r\nTransfer-Encoding: chunked\r\n"
            "Connection: close\r\n\r\n100\r\n" +
            chunkData + "\r\n100\r\n" + chunkData + "\r\n0\r\n\r\n";
      }
      else {
        const std::string bodyJson =
            "{\"status\":404,\"info\":null,\"errors\":{\"code\":\"NOT_FOUND\","
            "\"message\":\"Path not found\"}}";
        response =
            "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n"
            "Content-Length: " +
            std::to_string(bodyJson.size()) +
            "\r\nConnection: close\r\n\r\n" + bodyJson;
      }

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
  std::map<std::string, int> requests_;
  mutable std::mutex mutex_;
  std::thread thread_;
};

} // namespace
