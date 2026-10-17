#include "recording-config.hxx"

#include <config/config-service.hxx>

#include <algorithm>

namespace
{
int64_t positiveOr(const char* key, int64_t fallback)
{
  const int64_t value = ConfigService::getInt(key);
  return value > 0 ? value : fallback;
}
}

RecordingConfig recording_config::resolve()
{
  RecordingConfig config;
  config.enabled = ConfigService::getBool("recording.enabled");
  config.preRollMs = positiveOr("recording.pre_roll_s", 10) * 1000;
  config.postRollMs = positiveOr("recording.post_roll_s", 10) * 1000;
  config.segmentMs = positiveOr("recording.segment_s", 120) * 1000;
  config.retentionDays =
      std::clamp<int64_t>(positiveOr("recording.retention_days", 7), 1, 90);
  config.rescanMs = positiveOr("recording.rescan_s", 15) * 1000;
  config.preRollBytes = static_cast<size_t>(
      std::clamp<int64_t>(positiveOr("recording.pre_roll_bytes", 12 * 1024 * 1024),
                          1024 * 1024, 256 * 1024 * 1024));
  config.maxClipBytes = static_cast<size_t>(
      std::clamp<int64_t>(positiveOr("recording.max_clip_bytes", 96 * 1024 * 1024),
                          4 * 1024 * 1024, 512 * 1024 * 1024));
  config.sweepIntervalMs = positiveOr("recording.sweep_hours", 24) * 3600 * 1000;
  config.sweepInitialDelayMs = positiveOr("recording.sweep_start_s", 60) * 1000;
  config.minClipMs = positiveOr("recording.min_clip_s", 1) * 1000;
  return config;
}
