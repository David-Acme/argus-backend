#pragma once

#include <cstdint>
#include <map>
#include <mutex>

struct GainNormalizeInput
{
  float snrDb{0.0F};
  float typicalDb{0.0F};
  float meanTypicalDb{0.0F};
  float weight{0.5F};
};

[[nodiscard]] float normalizedSnr(const GainNormalizeInput& input);

class CameraGains
{
public:
  explicit CameraGains(float alpha);

  void observe(int64_t cameraId, float snrDb);
  [[nodiscard]] float normalize(int64_t cameraId, float snrDb, float weight) const;
  [[nodiscard]] float typical(int64_t cameraId) const;

private:
  float alpha_;
  mutable std::mutex mutex_;
  std::map<int64_t, float> typical_;
};

struct SpeakerLevelSample
{
  int64_t cameraId{0};
  float snrDb{0.0F};
};

class SpeakerLevels
{
public:
  explicit SpeakerLevels(float alpha);

  void observe(const SpeakerLevelSample& sample);
  [[nodiscard]] std::map<int64_t, float> snapshot() const;
  [[nodiscard]] int64_t loudest() const;
  void clear();

private:
  float alpha_;
  mutable std::mutex mutex_;
  std::map<int64_t, float> levels_;
};
