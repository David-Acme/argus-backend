#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

// Options for FakeLlmServer's canned chat/stream responses.
struct FakeLlmOptions
{
  std::vector<std::string> tokens;
  int status{200};
  bool coalesce{false};
  bool truncated{false};
  int tokenDelayMs{0};
};

// Minimal in-process HTTP server standing in for the argus-llm wire in unit tests.
class FakeLlmServer
{
public:
  explicit FakeLlmServer(FakeLlmOptions options = {})
      : options_(options)
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

  ~FakeLlmServer()
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

  static void sendAll(int fd, const std::string& data)
  {
    size_t sent = 0;
    while (sent < data.size()) {
      const auto n = ::send(fd, data.data() + sent, data.size() - sent, 0);
      if (n <= 0)
        return;
      sent += static_cast<size_t>(n);
    }
  }

  static std::string chunk(const std::string& data)
  {
    std::string frame;
    char size[32];
    std::snprintf(size, sizeof(size), "%zx\r\n", data.size());
    frame += size;
    frame += data;
    frame += "\r\n";
    return frame;
  }

  static std::string sentinelLine()
  {
    return "{\"done\":true,\"prompt_tokens\":7,\"reused_tokens\":0,"
           "\"decoded_tokens\":7}";
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

      const std::string errorJson =
          "{\"status\":" + std::to_string(options_.status) +
          ",\"info\":null,\"errors\":{\"code\":\"LLM_NOT_LOADED\","
          "\"message\":\"engine down\"}}";

      if (options_.status != 200) {
        const std::string response =
            "HTTP/1.1 " + std::to_string(options_.status) +
            " Unavailable\r\nContent-Type: application/json\r\n"
            "Content-Length: " +
            std::to_string(errorJson.size()) +
            "\r\nConnection: close\r\n\r\n" + errorJson;
        sendAll(fd, response);
        ::close(fd);
        continue;
      }

      if (path == "/llm/v1/chat-stream") {
        const std::string head =
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
            "Transfer-Encoding: chunked\r\n\r\n";
        sendAll(fd, head);
        if (options_.truncated) {
          for (const auto& token : options_.tokens)
            sendAll(fd, chunk(token));
          ::close(fd);
          continue;
        }
        const std::string sentinel = "\n" + sentinelLine() + "\n";
        if (options_.coalesce) {
          std::string everything;
          for (const auto& token : options_.tokens)
            everything += token;
          sendAll(fd, chunk(everything + sentinel));
        }
        else {
          for (const auto& token : options_.tokens) {
            if (options_.tokenDelayMs > 0)
              std::this_thread::sleep_for(
                  std::chrono::milliseconds(options_.tokenDelayMs));
            sendAll(fd, chunk(token));
          }
          sendAll(fd, chunk(sentinel));
        }
        sendAll(fd, "0\r\n\r\n");
        ::close(fd);
        continue;
      }

      std::string text;
      for (const auto& token : options_.tokens)
        text += token;
      const std::string bodyJson =
          "{\"status\":200,\"info\":{\"text\":\"" + text +
          "\"},\"errors\":null}";
      const std::string response =
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
          "Content-Length: " +
          std::to_string(bodyJson.size()) +
          "\r\nConnection: close\r\n\r\n" + bodyJson;
      sendAll(fd, response);
      ::close(fd);
    }
  }

  FakeLlmOptions options_;
  int listen_{-1};
  int port_{0};
  mutable std::mutex mutex_;
  std::map<std::string, int> requests_;
  std::thread thread_;
};

} // namespace
