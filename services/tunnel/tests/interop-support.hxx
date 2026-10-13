#pragma once

#include "link-test-cert.hxx"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tunnel::test
{
class ChildProcess
{
public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  bool spawn(const std::vector<std::string>& arguments);
  void requestStop();
  [[nodiscard]] bool running() const;
  void terminate();

private:
  int pid_{-1};
};

class HomeTlsServer
{
public:
  struct Params
  {
    std::string certificatePath;
    std::string keyPath;
  };

  explicit HomeTlsServer(Params params);
  ~HomeTlsServer();
  HomeTlsServer(const HomeTlsServer&) = delete;
  HomeTlsServer& operator=(const HomeTlsServer&) = delete;

  [[nodiscard]] uint16_t port() const { return port_; }
  [[nodiscard]] size_t accepted() const { return accepted_.load(); }
  [[nodiscard]] size_t echoed() const { return echoed_.load(); }
  void stop();

private:
  void serve();

  Params params_;
  int listenFd_{-1};
  uint16_t port_{0};
  std::jthread worker_;
  std::atomic<bool> stopped_{false};
  std::atomic<size_t> accepted_{0};
  std::atomic<size_t> echoed_{0};
};

class TunnelApp
{
public:
  struct Params
  {
    std::string host{"127.0.0.1"};
    uint16_t port{0};
    std::string serverName;
    std::string pinnedCertificatePath;
    int timeoutMs{5000};
  };

  explicit TunnelApp(const Params& params);
  ~TunnelApp();
  TunnelApp(const TunnelApp&) = delete;
  TunnelApp& operator=(const TunnelApp&) = delete;

  [[nodiscard]] bool handshaken() const { return handshaken_; }
  [[nodiscard]] const std::string& lastError() const { return lastError_; }
  struct RequestInput
  {
    std::string_view bytes;
    size_t expected{0};
    std::chrono::milliseconds timeout{0};
  };

  bool write(std::string_view bytes);
  std::string read(size_t bytes, std::chrono::milliseconds timeout);
  [[nodiscard]] std::string request(const RequestInput& input);

private:
  int fd_{-1};
  void* context_{nullptr};
  void* ssl_{nullptr};
  bool handshaken_{false};
  std::string lastError_;
};

bool writeFile(const std::filesystem::path& path, std::string_view text);
bool waitForPort(uint16_t port, std::chrono::milliseconds timeout);
uint16_t freeLoopbackPort();
}
