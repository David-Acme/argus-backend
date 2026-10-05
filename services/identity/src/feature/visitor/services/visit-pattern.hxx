#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

struct VisitPatternSummary
{
  std::vector<int> weekdays;
  std::optional<int> usualHour;
  int visitsConsidered{0};
};

namespace visit_pattern
{
[[nodiscard]] VisitPatternSummary summarize(std::span<const int64_t> startedAt);
}
