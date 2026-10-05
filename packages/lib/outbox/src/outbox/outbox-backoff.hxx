#pragma once

#include <algorithm>
#include <chrono>

namespace outbox
{

inline constexpr int kRetryMs = 500;
inline constexpr int kMaxRetryMs = 5000;
inline constexpr int kProgressMs = 50;
inline constexpr int kBatch = 64;

struct OutboxTiming
{
  int retryMs{kRetryMs};
  int maxRetryMs{kMaxRetryMs};
  int progressMs{kProgressMs};
  int batch{kBatch};
};

[[nodiscard]] constexpr std::chrono::milliseconds
retryDelay(const OutboxTiming& timing, int failures)
{
  const long long ceiling = std::max(timing.retryMs, timing.maxRetryMs);
  long long delay = std::max(timing.retryMs, 1);
  for (int step = 1; step < failures && delay < ceiling; ++step)
    delay *= 2;
  return std::chrono::milliseconds(std::min(delay, ceiling));
}

}
