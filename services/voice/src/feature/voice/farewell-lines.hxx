#pragma once

#include <feature/voice/voice-engine-seam.hxx>
#include <voice/voice-lang.hxx>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

enum class FarewellReason : uint8_t
{
  SessionClosed,
  ClosedByOwner,
  AccountDisabled
};

struct FarewellKey
{
  VoiceLang lang{VoiceLang::Es};
  FarewellReason reason{FarewellReason::SessionClosed};
};

[[nodiscard]] FarewellReason farewellReasonOf(std::string_view cause);
[[nodiscard]] std::string_view farewellLine(const FarewellKey& key);

struct FarewellAudio
{
  std::string text;
  std::shared_ptr<const std::vector<int16_t>> pcm;
};

class FarewellCache
{
public:
  explicit FarewellCache(IVoiceTts& tts);
  FarewellCache(const FarewellCache&) = delete;
  FarewellCache& operator=(const FarewellCache&) = delete;
  FarewellCache(FarewellCache&&) = delete;
  FarewellCache& operator=(FarewellCache&&) = delete;
  ~FarewellCache();

  void startWarming();
  bool warmOnce(const std::stop_token& stop);
  [[nodiscard]] FarewellAudio audio(const FarewellKey& key) const;

  static constexpr int kSampleRate = 16000;
  static constexpr auto kRetryEvery = std::chrono::seconds(15);

private:
  static constexpr std::size_t kLines = 6;
  [[nodiscard]] static std::size_t slotOf(const FarewellKey& key);

  IVoiceTts& tts_;
  mutable std::mutex mutex_;
  std::array<std::shared_ptr<const std::vector<int16_t>>, kLines> pcm_{};
  std::jthread warmer_;
};
