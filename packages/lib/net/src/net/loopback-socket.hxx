#pragma once

#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>

namespace argus::net
{

class Socket
{
public:
  Socket() = default;
  explicit Socket(int fd) noexcept : fd_(fd) {}
  ~Socket();

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  void shutdown() const noexcept;

private:
  int fd_{-1};
};

enum class NetStatus : std::uint8_t
{
  Ok,
  Closed,
  Failed,
  Cancelled
};

struct Endpoint
{
  std::string host;
  int port{0};
};

[[nodiscard]] Endpoint parseEndpoint(std::string_view url);

struct ConnectInput
{
  std::string_view host;
  int port{0};
  std::chrono::milliseconds timeout{0};
  std::stop_token cancellation;
};

struct Connection
{
  Socket socket;
  NetStatus status{NetStatus::Failed};
  int error{0};

  [[nodiscard]] bool connected() const noexcept
  {
    return status == NetStatus::Ok;
  }
};

[[nodiscard]] Connection connectLoopback(const ConnectInput& input);

struct SendInput
{
  int fd{-1};
  std::string_view data;
  std::stop_token cancellation;
};

[[nodiscard]] NetStatus sendAll(const SendInput& input);

struct ReceiveInput
{
  int fd{-1};
  std::string& into;
  std::stop_token cancellation;
};

[[nodiscard]] NetStatus receiveSome(const ReceiveInput& input);

struct ReadInput
{
  int fd{-1};
  std::chrono::steady_clock::time_point deadline;
  std::stop_token cancellation;
};

struct ReadResult
{
  std::string data;
  NetStatus status{NetStatus::Closed};
};

[[nodiscard]] ReadResult readUntilClosed(const ReadInput& input);

}
