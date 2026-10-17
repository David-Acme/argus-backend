#include "clip-builder.hxx"

#include <algorithm>

namespace
{
constexpr int64_t kRingSlackMs = 2000;

std::string codecOfInit(const std::string& init)
{
  if (init.find("avc1") != std::string::npos ||
      init.find("avc3") != std::string::npos)
    return "h264";
  if (init.find("hvc1") != std::string::npos ||
      init.find("hev1") != std::string::npos)
    return "h265";
  if (init.find("av01") != std::string::npos)
    return "av1";
  return "unknown";
}
}

bool clip_recording::triggerAllowed(CameraRecordMode mode,
                                    CameraClipTrigger trigger)
{
  switch (mode) {
    case CameraRecordMode::Off:
      return false;
    case CameraRecordMode::Continuous:
      return true;
    case CameraRecordMode::Motion:
      return trigger == CameraClipTrigger::Motion;
    case CameraRecordMode::Events:
      return trigger == CameraClipTrigger::Event ||
             trigger == CameraClipTrigger::Motion;
  }
  return false;
}

void ClipBuilder::configure(const ClipBuilderConfig& config)
{
  config_ = config;
}

void ClipBuilder::setInit(const std::string& init)
{
  init_ = init;
  codec_ = codecOfInit(init);
  hasInit_ = !init.empty();
}

void ClipBuilder::reset()
{
  ring_.clear();
  ringBytes_ = 0;
  active_.reset();
  ready_.reset();
  init_.clear();
  codec_.clear();
  hasInit_ = false;
}

bool ClipBuilder::request(CameraClipTrigger trigger, int64_t atMs,
                           int64_t epochMs)
{
  if (active_.has_value()) {
    active_->postRollUntilMs =
        std::max(active_->postRollUntilMs, atMs + config_.postRollMs);
    return true;
  }
  begin(trigger, atMs, epochMs, config_.preRollMs);
  return true;
}

void ClipBuilder::begin(CameraClipTrigger trigger, int64_t atMs, int64_t epochMs,
                        int64_t preRollMs)
{
  Active clip;
  clip.trigger = trigger;
  clip.anchorSteadyMs = atMs;
  clip.anchorEpochMs = epochMs;
  clip.postRollUntilMs =
      trigger == CameraClipTrigger::Continuous ? atMs + config_.segmentMs
                                                : atMs + config_.postRollMs;
  clip.awaitingKeyframe = true;

  const int64_t windowStartMs = atMs - preRollMs;
  auto start = ring_.end();
  for (auto it = ring_.begin(); it != ring_.end(); ++it) {
    if (it->keyframe && it->atMs >= windowStartMs) {
      start = it;
      break;
    }
  }
  if (start != ring_.end()) {
    clip.awaitingKeyframe = false;
    for (auto it = start; it != ring_.end(); ++it) {
      clip.body += it->bytes;
      clip.bytes += it->bytes.size();
      clip.firstAtMs = clip.firstAtMs == 0 ? it->atMs : clip.firstAtMs;
      clip.lastAtMs = it->atMs;
    }
    clip.anchorEpochMs = epochMs - (atMs - clip.firstAtMs);
  }
  active_ = std::move(clip);
}

void ClipBuilder::push(const ClipChunkInput& input)
{
  ring_.push_back(
      {.bytes = std::string(reinterpret_cast<const char*>(input.payload),
                            input.len),
       .keyframe = input.keyframe,
       .atMs = input.atMs});
  ringBytes_ += input.len;
  const int64_t windowStartMs = input.atMs - config_.preRollMs - kRingSlackMs;
  while (!ring_.empty() &&
         (ringBytes_ > config_.preRollBytes || ring_.front().atMs < windowStartMs)) {
    ringBytes_ -= ring_.front().bytes.size();
    ring_.pop_front();
  }

  if (!active_.has_value())
    return;
  Active& clip = *active_;
  if (clip.awaitingKeyframe) {
    if (!input.keyframe)
      return;
    clip.awaitingKeyframe = false;
    clip.body.clear();
    clip.bytes = 0;
    clip.firstAtMs = input.atMs;
    clip.lastAtMs = input.atMs;
    clip.anchorEpochMs = clip.anchorEpochMs + (input.atMs - clip.anchorSteadyMs);
    clip.anchorSteadyMs = input.atMs;
  }
  if (clip.bytes + input.len > config_.maxClipBytes)
    clip.postRollUntilMs = input.atMs;
  else {
    clip.body.append(reinterpret_cast<const char*>(input.payload), input.len);
    clip.bytes += input.len;
    clip.firstAtMs = clip.firstAtMs == 0 ? input.atMs : clip.firstAtMs;
    clip.lastAtMs = input.atMs;
  }
  if (input.atMs >= clip.postRollUntilMs)
    close();
}

void ClipBuilder::close()
{
  if (!active_.has_value())
    return;
  const Active clip = std::move(*active_);
  const bool continuous = clip.trigger == CameraClipTrigger::Continuous;
  active_.reset();
  if (!clip.body.empty() && hasInit_) {
    ReadyClip ready;
    ready.trigger = clip.trigger;
    ready.codec = codec_;
    ready.body = init_ + clip.body;
    ready.startedAtMs = clip.anchorEpochMs + (clip.firstAtMs - clip.anchorSteadyMs);
    ready.durationMs = std::max<int64_t>(0, clip.lastAtMs - clip.firstAtMs);
    ready_ = std::move(ready);
  }
  if (continuous) {
    begin(CameraClipTrigger::Continuous, clip.lastAtMs,
          clip.anchorEpochMs + (clip.lastAtMs - clip.anchorSteadyMs), 0);
  }
}

std::optional<ReadyClip> ClipBuilder::takeReady()
{
  std::optional<ReadyClip> ready = std::move(ready_);
  ready_.reset();
  return ready;
}
