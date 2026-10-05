#include "visit-pattern.hxx"

#include <algorithm>
#include <array>
#include <ctime>

namespace
{
constexpr int kMinVisits = 3;
constexpr double kWeekdayShare = 0.3;
constexpr int kHourSpread = 3;
}

VisitPatternSummary visit_pattern::summarize(std::span<const int64_t> startedAt)
{
  VisitPatternSummary summary;
  summary.visitsConsidered = static_cast<int>(startedAt.size());
  if (summary.visitsConsidered < kMinVisits)
    return summary;
  std::array<int, 7> perWeekday{};
  std::vector<int> hours;
  hours.reserve(startedAt.size());
  for (const int64_t at : startedAt) {
    const std::time_t stamp = at;
    std::tm local{};
    localtime_r(&stamp, &local);
    ++perWeekday.at(static_cast<std::size_t>(local.tm_wday));
    hours.push_back(local.tm_hour);
  }
  const int threshold = std::max(
      2, static_cast<int>(kWeekdayShare * summary.visitsConsidered + 0.999));
  for (int day = 0; day < 7; ++day)
    if (perWeekday.at(static_cast<std::size_t>(day)) >= threshold)
      summary.weekdays.push_back(day);
  std::ranges::sort(hours);
  const auto quartile = [&](double q) {
    return hours.at(static_cast<std::size_t>(q * static_cast<double>(hours.size() - 1)));
  };
  if (quartile(0.75) - quartile(0.25) <= kHourSpread)
    summary.usualHour = quartile(0.5);
  return summary;
}
