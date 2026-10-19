#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
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

struct StreamUpdate
{
  std::string text;
  bool final{false};
  std::size_t samples{0};
  int decodeMs{0};
};

struct StreamInput
{
  int sampleRate{16000};
  std::string language;
  std::function<void(const StreamUpdate&)> onPartial;
  std::stop_token cancellation;
};

class TranscribeStream
{
public:
  struct Impl;
  explicit TranscribeStream(std::unique_ptr<Impl> impl);
  ~TranscribeStream();
  TranscribeStream(const TranscribeStream&) = delete;
  TranscribeStream& operator=(const TranscribeStream&) = delete;
  void push(std::span<const float> samples);
  void flush();
  [[nodiscard]] StreamUpdate finish();
  void cancel();
private:
  std::unique_ptr<Impl> impl_;
};

struct SpotHitEvent
{
  std::string keyword;
  std::uint64_t atSample{0};
  float startTime{0.0F};
};

struct SpotInput
{
  int sampleRate{16000};
  std::function<void(const SpotHitEvent&)> onHit;
};

class SpotStream
{
public:
  struct Impl;
  explicit SpotStream(std::unique_ptr<Impl> impl);
  ~SpotStream();
  SpotStream(const SpotStream&) = delete;
  SpotStream& operator=(const SpotStream&) = delete;
  [[nodiscard]] bool push(std::span<const float> samples);
  [[nodiscard]] bool reset();
  [[nodiscard]] bool alive() const;
  void close();
private:
  std::unique_ptr<Impl> impl_;
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
  [[nodiscard]] std::unique_ptr<TranscribeStream> openStream(StreamInput input) const;
  [[nodiscard]] std::unique_ptr<SpotStream> openSpot(SpotInput input) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
