#include "echo-guard.hxx"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace
{

int64_t msBetween(WatchClock::time_point from, WatchClock::time_point to)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

float pearson(std::span<const float> left, std::span<const float> right)
{
  const size_t count = std::min(left.size(), right.size());
  if (count < 2)
    return 0.0F;
  const double meanLeft = std::accumulate(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(count), 0.0) /
                          static_cast<double>(count);
  const double meanRight =
      std::accumulate(right.begin(), right.begin() + static_cast<std::ptrdiff_t>(count), 0.0) /
      static_cast<double>(count);
  double cross = 0.0;
  double leftSquares = 0.0;
  double rightSquares = 0.0;
  for (size_t index = 0; index < count; ++index) {
    const double a = left[index] - meanLeft;
    const double b = right[index] - meanRight;
    cross += a * b;
    leftSquares += a * a;
    rightSquares += b * b;
  }
  if (leftSquares <= 0.0 || rightSquares <= 0.0)
    return 0.0F;
  return static_cast<float>(cross / std::sqrt(leftSquares * rightSquares));
}

}

std::vector<float> envelopeOf(std::span<const float> samples, size_t frameSamples)
{
  std::vector<float> envelope;
  if (frameSamples == 0)
    return envelope;
  envelope.reserve(samples.size() / frameSamples);
  for (size_t start = 0; start + frameSamples <= samples.size(); start += frameSamples) {
    double sum = 0.0;
    for (size_t index = start; index < start + frameSamples; ++index)
      sum += static_cast<double>(samples[index]) * samples[index];
    envelope.push_back(static_cast<float>(std::sqrt(sum / static_cast<double>(frameSamples))));
  }
  return envelope;
}

float alignedCorrelation(const AlignedEnvelopes& input)
{
  if (input.frameMs <= 0 || input.mic.empty() || input.played.empty())
    return 0.0F;
  const int64_t maxLagFrames = input.maxLagMs / input.frameMs;
  float best = 0.0F;
  for (int64_t lag = 0; lag <= maxLagFrames; ++lag) {
    const int64_t offset = ((input.micStartMs - input.playedStartMs) / input.frameMs) - lag;
    int64_t micFirst = 0;
    int64_t playedFirst = offset;
    if (playedFirst < 0) {
      micFirst = -playedFirst;
      playedFirst = 0;
    }
    if (micFirst >= static_cast<int64_t>(input.mic.size()) ||
        playedFirst >= static_cast<int64_t>(input.played.size()))
      continue;
    const auto overlap = static_cast<size_t>(std::min(static_cast<int64_t>(input.mic.size()) - micFirst,
                                                      static_cast<int64_t>(input.played.size()) - playedFirst));
    if (overlap < input.minOverlap)
      continue;
    best = std::max(best, pearson(input.mic.subspan(static_cast<size_t>(micFirst), overlap),
                                  input.played.subspan(static_cast<size_t>(playedFirst), overlap)));
  }
  return best;
}

EchoGuard::EchoGuard(EchoGuardConfig config) : config_(config) {}

void EchoGuard::prune(CameraPlayback& playback, WatchClock::time_point now) const
{
  while (!playback.history.empty() && playback.history.size() > 1 &&
         now - playback.history.front().start > config_.history)
    playback.history.pop_front();
}

void EchoGuard::beginPlayback(int64_t cameraId, WatchClock::time_point at)
{
  std::scoped_lock lock(mutex_);
  CameraPlayback& playback = cameras_[cameraId];
  playback.playing = true;
  playback.history.push_back({.start = at, .envelope = {}, .partial = {}});
  prune(playback, at);
}

void EchoGuard::addPlayed(int64_t cameraId, std::span<const float> samples, WatchClock::time_point at)
{
  const size_t frame = static_cast<size_t>(config_.sampleRate) * kEnvelopeFrameMs / 1000;
  std::scoped_lock lock(mutex_);
  CameraPlayback& playback = cameras_[cameraId];
  if (playback.history.empty())
    playback.history.push_back({.start = at, .envelope = {}, .partial = {}});
  Played& played = playback.history.back();
  played.partial.insert(played.partial.end(), samples.begin(), samples.end());
  const std::vector<float> more = envelopeOf(played.partial, frame);
  played.envelope.insert(played.envelope.end(), more.begin(), more.end());
  played.partial.erase(played.partial.begin(),
                       played.partial.begin() + static_cast<std::ptrdiff_t>(more.size() * frame));
}

void EchoGuard::endPlayback(int64_t cameraId, WatchClock::time_point end)
{
  std::scoped_lock lock(mutex_);
  CameraPlayback& playback = cameras_[cameraId];
  playback.playing = false;
  playback.end = end;
}

bool EchoGuard::muted(int64_t cameraId, WatchClock::time_point now) const
{
  std::scoped_lock lock(mutex_);
  const auto found = cameras_.find(cameraId);
  if (found == cameras_.end())
    return false;
  return found->second.playing || now < found->second.end + config_.tail;
}

bool EchoGuard::othersPlaying(const EchoQuery& query, WatchClock::time_point now) const
{
  std::scoped_lock lock(mutex_);
  return std::ranges::any_of(cameras_, [&](const auto& entry) {
    if (entry.first == query.cameraId)
      return false;
    if (query.sameEnvironment && !query.sameEnvironment(query.cameraId, entry.first))
      return false;
    return entry.second.playing || now < entry.second.end + config_.tail;
  });
}

float EchoGuard::correlationWithOthers(const EchoQuery& query) const
{
  std::scoped_lock lock(mutex_);
  float best = 0.0F;
  for (const auto& [cameraId, playback] : cameras_) {
    if (cameraId == query.cameraId)
      continue;
    if (query.sameEnvironment && !query.sameEnvironment(query.cameraId, cameraId))
      continue;
    for (const Played& played : playback.history) {
      best = std::max(best, alignedCorrelation({.mic = query.micEnvelope,
                                                .micStartMs = msBetween(played.start, query.micStart),
                                                .played = played.envelope,
                                                .playedStartMs = 0,
                                                .frameMs = kEnvelopeFrameMs,
                                                .maxLagMs = config_.maxLag.count(),
                                                .minOverlap = 25}));
    }
  }
  return best;
}
