#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr const char* kConfigBody =
    R"({"status":200,"info":{"sampleRate":22050,"defaultSpeed":1.25},"errors":null})";

struct FakeTtsStreamChunk
{
  std::chrono::milliseconds delay{0};
  bool stop{false};
};

// Minimal in-process HTTP server standing in for the argus-tts wire in unit tests.
class FakeTtsServer
{
public:
  explicit FakeTtsServer(int status = 200,
                         std::vector<FakeTtsStreamChunk> streamChunks = {})
      : status_(status),
        streamChunks_(std::move(streamChunks))
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

  void delayConfig(std::chrono::milliseconds delay)
  {
    configDelayMs_.store(delay.count());
  }

  void stop()
  {
    if (listen_ < 0)
      return;
    stopping_.store(true);
    ::shutdown(listen_, SHUT_RDWR);
    if (thread_.joinable())
      thread_.join();
    ::close(listen_);
    listen_ = -1;
  }

private:
  static bool sendResponse(int fd, std::string_view response)
  {
    size_t sent = 0;
    while (sent < response.size()) {
      const auto n = ::send(fd, response.data() + sent,
                            response.size() - sent, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      sent += static_cast<size_t>(n);
    }
    return true;
  }

  // Reads until the head terminator, then the Content-Length body if any.
  static std::string readRequest(int fd)
  {
    std::string data;
    std::array<char, 4096> buffer{};
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
      const auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (n <= 0)
        break;
      data.append(buffer.data(), static_cast<size_t>(n));
    }
    for (;;) {
      const auto need = bodyStart();
      if (need == std::string::npos || data.size() >= need)
        break;
      const auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (n <= 0)
        break;
      data.append(buffer.data(), static_cast<size_t>(n));
    }
    return data;
  }

  void serve()
  {
    while (!stopping_.load()) {
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
        std::this_thread::sleep_for(std::chrono::milliseconds(configDelayMs_.load()));
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
        std::string header =
            "HTTP/1.1 200 OK\r\nContent-Type: audio/x-argus-pcm-f32\r\n"
            "X-Argus-Sample-Rate: 22050\r\nTransfer-Encoding: chunked\r\n"
            "Connection: close\r\n\r\n";
        if (streamChunks_.empty()) {
          std::string body = "100\r\n" + chunkData + "\r\n100\r\n" + chunkData + "\r\n0\r\n\r\n";
          std::string response = header + body;
          size_t sent = 0;
          while (sent < response.size()) {
            const auto n = ::send(fd, response.data() + sent,
                                  response.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
              continue;
            if (n <= 0)
              break;
            sent += static_cast<size_t>(n);
          }
          ::close(fd);
          continue;
        }
        // Send headers first, then each chunk with its delay.
        {
          size_t sent = 0;
          while (sent < header.size()) {
            const auto n = ::send(fd, header.data() + sent,
                                  header.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
              continue;
            if (n <= 0)
              break; // peer closed; nothing more to send
            sent += static_cast<size_t>(n);
          }
          if (sent < header.size()) {
            ::close(fd);
            continue;
          }
        }
        for (size_t i = 0; i < streamChunks_.size(); ++i) {
          const auto& spec = streamChunks_[i];
          if (spec.delay.count() > 0)
            std::this_thread::sleep_for(spec.delay);
          if (spec.stop)
            break;
          std::string chunkBody = "64\r\n" + chunkData.substr(0, 64 * 4) + "\r\n";
          size_t sent = 0;
          bool peerClosed = false;
          while (sent < chunkBody.size()) {
            const auto n = ::send(fd, chunkBody.data() + sent,
                                  chunkBody.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
              continue;
            if (n <= 0) {
              peerClosed = true;
              break;
            }
            sent += static_cast<size_t>(n);
          }
          if (peerClosed)
            break;
        }
        if (!streamChunks_.back().stop) {
          std::string terminal = "0\r\n\r\n";
          size_t sent = 0;
          while (sent < terminal.size()) {
            const auto n = ::send(fd, terminal.data() + sent,
                                  terminal.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
              continue;
            if (n <= 0)
              break; // peer closed
            sent += static_cast<size_t>(n);
          }
        }
        ::close(fd);
        continue;
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
  std::vector<FakeTtsStreamChunk> streamChunks_;
  std::atomic<std::chrono::milliseconds::rep> configDelayMs_{0};
  std::atomic<bool> stopping_{false};
  std::map<std::string, int> requests_;
  mutable std::mutex mutex_;
  std::thread thread_;
};

} // namespace
