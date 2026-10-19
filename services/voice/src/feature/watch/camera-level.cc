#include "camera-level.hxx"

#include <shared/services/vad/near-field-gate.hxx>

#include <algorithm>
#include <cmath>

CameraLevel::CameraLevel(CameraLevelConfig config) : config_(config), floorDb_(config.initialFloorDb) {}

void CameraLevel::observe(std::span<const float> frame, bool speech)
{
  if (frame.empty() || config_.sampleRate <= 0)
    return;
  const float db = rmsDbOf(frame);
  lastDb_ = db;
  const float seconds = static_cast<float>(frame.size()) / static_cast<float>(config_.sampleRate);
  if (!speech) {
    if (db < floorDb_)
      floorDb_ = std::max(db, floorDb_ - (config_.floorFallDbPerSecond * seconds));
    else
      floorDb_ = std::min(db, floorDb_ + (config_.floorRiseDbPerSecond * seconds));
  }
  window_.push_back({.db = db, .speech = speech});
  if (windowFrames_ == 0)
    windowFrames_ = std::max<size_t>(1, static_cast<size_t>(config_.window.count()) * static_cast<size_t>(config_.sampleRate) /
                                            (1000 * frame.size()));
  while (window_.size() > windowFrames_)
    window_.pop_front();
}

float CameraLevel::speechSnrDb() const
{
  double sum = 0.0;
  size_t count = 0;
  for (const LevelFrame& frame : window_) {
    if (!frame.speech)
      continue;
    sum += frame.db;
    ++count;
  }
  if (count == 0)
    return 0.0F;
  return static_cast<float>(sum / static_cast<double>(count)) - floorDb_;
}
