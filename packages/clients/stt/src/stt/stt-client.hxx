#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace argus::stt
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
  int sampleRate{0};
  bool loaded{false};
  std::string language;
  std::string defaultLanguage;
  std::vector<std::string> languages;
};

struct TranscribeInput
{
  std::vector<float> samples;
  int sampleRate{16000};
  std::string language;
  std::stop_token cancellation;
};

class Client
{
public:
  explicit Client(ClientConfig config);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  [[nodiscard]] Capabilities capabilities(
      const std::stop_token& cancellation = {}) const;
  [[nodiscard]] std::string transcribe(const TranscribeInput& input) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
