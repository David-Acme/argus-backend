#include "camera-gains.hxx"

#include <algorithm>

float normalizedSnr(const GainNormalizeInput& input)
{
  return input.snrDb - (input.weight * (input.typicalDb - input.meanTypicalDb));
}

CameraGains::CameraGains(float alpha) : alpha_(std::clamp(alpha, 0.01F, 1.0F)) {}

void CameraGains::observe(int64_t cameraId, float snrDb)
{
  std::scoped_lock lock(mutex_);
  const auto found = typical_.find(cameraId);
  if (found == typical_.end())
    typical_.emplace(cameraId, snrDb);
  else
    found->second += alpha_ * (snrDb - found->second);
}

float CameraGains::typical(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = typical_.find(cameraId);
  return found == typical_.end() ? 0.0F : found->second;
}

float CameraGains::normalize(int64_t cameraId, float snrDb, float weight) const
{
  std::scoped_lock lock(mutex_);
  const auto found = typical_.find(cameraId);
  if (found == typical_.end() || typical_.size() < 2)
    return snrDb;
  float sum = 0.0F;
  for (const auto& [id, value] : typical_)
    sum += value;
  const float mean = sum / static_cast<float>(typical_.size());
  return normalizedSnr({.snrDb = snrDb, .typicalDb = found->second, .meanTypicalDb = mean, .weight = weight});
}

SpeakerLevels::SpeakerLevels(float alpha) : alpha_(std::clamp(alpha, 0.01F, 1.0F)) {}

void SpeakerLevels::observe(const SpeakerLevelSample& sample)
{
  std::scoped_lock lock(mutex_);
  const auto found = levels_.find(sample.cameraId);
  if (found == levels_.end())
    levels_.emplace(sample.cameraId, sample.snrDb);
  else
    found->second += alpha_ * (sample.snrDb - found->second);
}

std::map<int64_t, float> SpeakerLevels::snapshot() const
{
  std::scoped_lock lock(mutex_);
  return levels_;
}

int64_t SpeakerLevels::loudest() const
{
  std::scoped_lock lock(mutex_);
  const auto best = std::ranges::max_element(levels_, {}, [](const auto& entry) { return entry.second; });
  return best == levels_.end() ? 0 : best->first;
}

void SpeakerLevels::clear()
{
  std::scoped_lock lock(mutex_);
  levels_.clear();
}
