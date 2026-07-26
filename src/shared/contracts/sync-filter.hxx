#pragma once

#include <cstdint>
#include <optional>

struct SyncFilter
{
  std::optional<int64_t> startTime;
  std::optional<int64_t> endTime;
};
