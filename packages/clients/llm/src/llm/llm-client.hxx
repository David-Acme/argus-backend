#pragma once

#include <chrono>
#include <cstdint>
#include <llm/llm-service.hxx>
#include <memory>
#include <string>

namespace argus::llm
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
  int32_t defaultMaxTokens{0};
  float defaultTemperature{-1.0F};
  int64_t contextSize{0};
  int32_t lastPromptTokens{0};
  int32_t lastReusedTokens{0};
  int32_t lastDecodedTokens{0};
};

class Client
{
public:
  explicit Client(ClientConfig config);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  [[nodiscard]] Capabilities capabilities() const;
  [[nodiscard]] std::string chat(const ChatRequest& request) const;
  void chatStream(const LlmStreamInput& input) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
