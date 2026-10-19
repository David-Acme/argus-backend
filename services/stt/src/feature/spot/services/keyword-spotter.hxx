#pragma once

#include <config/stt-config.hxx>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

struct SherpaOnnxKeywordSpotter;

struct SpotHit
{
  std::string keyword;
  uint64_t atSample{0};
  float startTime{0.0F};
};

class SpotSession
{
public:
  virtual ~SpotSession() = default;

  [[nodiscard]] virtual std::vector<SpotHit> push(std::span<const float> samples, int sampleRate) = 0;
  virtual void reset() = 0;
};

class KeywordSpotterService
{
public:
  KeywordSpotterService();
  ~KeywordSpotterService();

  KeywordSpotterService(const KeywordSpotterService&) = delete;
  KeywordSpotterService& operator=(const KeywordSpotterService&) = delete;

  bool init(const SttSpotConfig& config);

  [[nodiscard]] bool loaded() const { return loaded_.load(); }

  [[nodiscard]] std::unique_ptr<SpotSession> open();

private:
  std::unique_ptr<const SherpaOnnxKeywordSpotter, void (*)(const SherpaOnnxKeywordSpotter*)> spotter_;
  std::shared_ptr<std::mutex> decodeMutex_{std::make_shared<std::mutex>()};
  std::atomic<bool> loaded_{false};
};
