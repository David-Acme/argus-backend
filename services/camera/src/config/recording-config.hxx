#pragma once

#include <cstddef>
#include <cstdint>

struct RecordingConfig
{
  bool enabled{true};
  int64_t preRollMs{10000};
  int64_t postRollMs{10000};
  int64_t segmentMs{120000};
  int64_t retentionDays{7};
  int64_t minRetentionDays{1};
  int64_t maxRetentionDays{90};
  int64_t rescanMs{15000};
  size_t preRollBytes{size_t{12} * 1024 * 1024};
  size_t maxClipBytes{size_t{96} * 1024 * 1024};
  int64_t sweepIntervalMs{24LL * 3600 * 1000};
  int64_t sweepInitialDelayMs{60000};
  int64_t minClipMs{1000};
};

namespace recording_config
{
[[nodiscard]] RecordingConfig resolve();
}
