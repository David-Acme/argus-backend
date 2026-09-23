#pragma once

#include <cstdint>

namespace stream_retention
{
inline constexpr int64_t kRetentionMs = 7LL * 24 * 60 * 60 * 1000;
inline constexpr int64_t kRetentionSeconds = kRetentionMs / 1000;
inline constexpr int64_t kRetentionNs = kRetentionMs * 1000000;
inline constexpr int64_t kDuplicatesNs = 2LL * 60 * 1000000000;
inline constexpr int64_t kSettledPurgeIntervalMs = 24LL * 60 * 60 * 1000;
inline constexpr int64_t kSettledPurgeRetryMs = 60LL * 60 * 1000;
}
