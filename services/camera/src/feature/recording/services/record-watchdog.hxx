#pragma once

#include <cstdint>

namespace record_watchdog
{

struct Limits
{
  int64_t assumedIntervalMs{3000};
  int64_t backoffMinMs{5000};
  int64_t backoffMaxMs{60000};
};

struct Slot
{
  int64_t lastFragmentMs{0};
  int64_t keyframeIntervalMs{0};
  int64_t retryAtMs{0};
  int64_t retryBackoffMs{0};
  bool stalled{false};
};

struct Verdict
{
  bool stalled{false};
  bool warn{false};
  bool retry{false};
  int64_t backoffMs{0};
  int64_t retryAtMs{0};
};

[[nodiscard]] Verdict verdict(const Slot& slot, int64_t nowMs,
                              const Limits& limits);

struct Interval
{
  int64_t lastKeyframeMs{0};
  int64_t intervalMs{0};
};

void observeKeyframe(Interval& interval, int64_t atMs);

}
