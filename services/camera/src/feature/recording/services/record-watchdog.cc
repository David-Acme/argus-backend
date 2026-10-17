#include "record-watchdog.hxx"

#include <algorithm>

namespace record_watchdog
{

Verdict verdict(const Slot& slot, int64_t nowMs, const Limits& limits)
{
  const int64_t interval =
      slot.keyframeIntervalMs > 0 ? slot.keyframeIntervalMs
                                  : limits.assumedIntervalMs;
  if (slot.lastFragmentMs > 0 && nowMs - slot.lastFragmentMs < 2 * interval)
    return {};
  Verdict out;
  out.stalled = true;
  out.warn = !slot.stalled;
  if (nowMs < slot.retryAtMs)
    return out;
  out.retry = true;
  out.backoffMs =
      slot.retryBackoffMs == 0
          ? limits.backoffMinMs
          : std::min(slot.retryBackoffMs * 2, limits.backoffMaxMs);
  out.retryAtMs = nowMs + out.backoffMs;
  return out;
}

void observeKeyframe(Interval& interval, int64_t atMs)
{
  if (interval.lastKeyframeMs > 0)
  {
    const int64_t sample = atMs - interval.lastKeyframeMs;
    if (sample > 0)
      interval.intervalMs = interval.intervalMs == 0
                                ? sample
                                : (interval.intervalMs * 3 + sample) / 4;
  }
  interval.lastKeyframeMs = atMs;
}

}
