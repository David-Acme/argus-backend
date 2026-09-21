#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace argus::tts
{
enum class Quality { Auto, Low, Medium, High };

struct ClientConfig
{
  std::string target;
  std::string credential;
  std::chrono::milliseconds timeout{30000};
};

struct Capabilities
{
  int sampleRate{0};
  int channels{1};
  float defaultSpeed{1.0F};
  std::vector<std::string> voices;
  std::vector<std::string> languages;
};

struct AudioChunk
{
  int sampleRate{0};
  std::uint64_t sequence{0};
  std::vector<float> samples;
};

struct SynthesisInput
{
  std::string text;
  std::string voice{"M3"};
  std::string language{"en"};
  float speed{0};
  Quality quality{Quality::Auto};
  std::stop_token cancellation;
  std::function<bool(AudioChunk)> onChunk;
};

class Client
{
public:
  explicit Client(ClientConfig config);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Capabilities capabilities(std::stop_token cancellation = {}) const;
  void synthesize(const SynthesisInput& input) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
