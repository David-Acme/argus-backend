#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SeriesWindowInput
{
  std::optional<std::string_view> rule;
  int64_t startsAt{0};
  std::optional<int64_t> endsAt;
  std::string_view exceptions;
  int64_t from{0};
  int64_t to{0};
};

struct SeriesOccurrence
{
  int64_t occurrence{0};
  int64_t startsAt{0};
  std::optional<int64_t> endsAt;
  std::optional<std::string> title;
  std::optional<std::string> location;
  bool repeats{false};
};

inline constexpr std::size_t kMaxSeriesOccurrences = 512;

[[nodiscard]] std::vector<SeriesOccurrence> occurrencesWithin(const SeriesWindowInput& input);
