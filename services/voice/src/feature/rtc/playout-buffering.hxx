#pragma once

#include <shared/wrapper/audio/sample-ring.hxx>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

inline constexpr int kPlayoutPreRollMs = 400;
inline constexpr int kLiveKitSourceQueueMs = 160;
inline constexpr std::chrono::milliseconds kPlayoutTailFlushAfter{200};

inline size_t tailPlayoutSamples(size_t buffered, size_t frameSamples)
{
  return std::min(buffered, frameSamples);
}

template <typename Ring>
size_t takeTailFrame(Ring& ring, std::span<int16_t> frame)
{
  const size_t tail = tailPlayoutSamples(ring.size(), frame.size());
  ring.pop(frame.data(), tail);
  std::fill(frame.begin() + static_cast<std::ptrdiff_t>(tail), frame.end(), int16_t{0});
  return tail;
}

class PlayoutBuffering
{
public:
  PlayoutBuffering(size_t preRollSamples, std::chrono::milliseconds tailFlushAfter)
      : preRollSamples_(preRollSamples), tailFlushAfter_(tailFlushAfter)
  {
  }

  [[nodiscard]] bool startsPlayout(size_t buffered)
  {
    if (!preRolled_ && buffered >= preRollSamples_)
      preRolled_ = true;
    return preRolled_;
  }

  [[nodiscard]] bool tailDue(std::chrono::steady_clock::duration sinceLastChunk) const
  {
    return sinceLastChunk > tailFlushAfter_;
  }

  void drained() { preRolled_ = false; }

  [[nodiscard]] bool preRolled() const { return preRolled_; }

private:
  size_t preRollSamples_;
  std::chrono::milliseconds tailFlushAfter_;
  bool preRolled_{false};
};
