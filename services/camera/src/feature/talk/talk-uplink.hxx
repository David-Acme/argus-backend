#pragma once

#include <audio/audio-resampler.hxx>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

struct TalkUplinkConfig
{
  int sourceRate{16000};
  int packetMs{120};
  int maxQueuedMs{1000};
};

class TalkUplink
{
public:
  static constexpr int kLineRate = 8000;

  explicit TalkUplink(TalkUplinkConfig config);

  void push(std::span<const int16_t> samples);
  std::optional<std::vector<int16_t>> next();

  [[nodiscard]] int queuedMs() const;
  [[nodiscard]] size_t packetSamples() const { return packetSamples_; }
  [[nodiscard]] int64_t droppedMs() const { return droppedSamples_ * 1000 / kLineRate; }

private:
  TalkUplinkConfig config_;
  AudioResampler resampler_;
  size_t packetSamples_;
  size_t maxQueued_;
  std::deque<int16_t> queue_;
  int64_t droppedSamples_{0};
};

namespace talk_frame
{
inline constexpr uint8_t kMagic = 0xA8;
inline constexpr uint8_t kAudio = 0x01;
inline constexpr size_t kHeaderBytes = 4;
inline constexpr size_t kMaxFrameBytes = size_t{32} * 1024;

std::optional<std::vector<int16_t>> parse(std::span<const uint8_t> frame);
}
