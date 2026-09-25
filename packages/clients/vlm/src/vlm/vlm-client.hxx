#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace argus::vlm
{
inline constexpr std::chrono::seconds kMaxTimeout{120};

struct ClientConfig
{
  std::string target;
  std::string credential;
  std::chrono::milliseconds timeout{30000};
};

struct Capabilities
{
  bool loaded{false};
  int maxInputPx{0};
  int defaultMaxTokens{0};
};

struct DescribeInput
{
  std::string jpeg;
  std::string prompt;
  std::string cameraId;
  int maxTokens{0};
};

class Client
{
public:
  explicit Client(ClientConfig config);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  [[nodiscard]] Capabilities capabilities() const;
  [[nodiscard]] std::string describe(const DescribeInput& input) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
