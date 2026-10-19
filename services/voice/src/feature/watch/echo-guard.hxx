#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

using WatchClock = std::chrono::steady_clock;

inline constexpr int kEnvelopeFrameMs = 20;

[[nodiscard]] std::vector<float> envelopeOf(std::span<const float> samples, size_t frameSamples);

struct AlignedEnvelopes
{
  std::span<const float> mic;
  int64_t micStartMs{0};
  std::span<const float> played;
  int64_t playedStartMs{0};
  int64_t frameMs{kEnvelopeFrameMs};
  int64_t maxLagMs{1000};
  size_t minOverlap{25};
};

[[nodiscard]] float alignedCorrelation(const AlignedEnvelopes& input);

struct EchoGuardConfig
{
  std::chrono::milliseconds tail{800};
  std::chrono::milliseconds maxLag{1000};
  std::chrono::milliseconds history{10000};
  int sampleRate{16000};
};

struct EchoQuery
{
  int64_t cameraId{0};
  std::span<const float> micEnvelope;
  WatchClock::time_point micStart{};
  std::function<bool(int64_t, int64_t)> sameEnvironment;
};

class EchoGuard
{
public:
  explicit EchoGuard(EchoGuardConfig config);

  void beginPlayback(int64_t cameraId, WatchClock::time_point at);
  void addPlayed(int64_t cameraId, std::span<const float> samples, WatchClock::time_point at);
  void endPlayback(int64_t cameraId, WatchClock::time_point end);

  [[nodiscard]] bool muted(int64_t cameraId, WatchClock::time_point now) const;
  [[nodiscard]] bool othersPlaying(const EchoQuery& query, WatchClock::time_point now) const;
  [[nodiscard]] float correlationWithOthers(const EchoQuery& query) const;

private:
  struct Played
  {
    WatchClock::time_point start{};
    std::vector<float> envelope;
    std::vector<float> partial;
  };

  struct CameraPlayback
  {
    bool playing{false};
    WatchClock::time_point end{};
    std::deque<Played> history;
  };

  void prune(CameraPlayback& playback, WatchClock::time_point now) const;

  EchoGuardConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<int64_t, CameraPlayback> cameras_;
};
