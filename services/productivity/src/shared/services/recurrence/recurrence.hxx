#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class RecurrenceFrequency : std::uint8_t
{
  Daily,
  Weekly,
  Monthly,
  Yearly
};

struct RecurrenceRule
{
  RecurrenceFrequency frequency{RecurrenceFrequency::Daily};
  int interval{1};
};

struct OccurrenceInput
{
  RecurrenceRule rule;
  int64_t anchor{0};
  int64_t index{0};
};

struct FirstOccurrenceInput
{
  RecurrenceRule rule;
  int64_t anchor{0};
  int64_t moment{0};
};

inline constexpr std::string_view kRecurrenceRuleProblem =
    "recurrenceRule must be FREQ=DAILY, WEEKLY, MONTHLY or YEARLY, with an optional INTERVAL from 1 to 999";

[[nodiscard]] std::optional<RecurrenceRule> parseRecurrenceRule(std::string_view text);
[[nodiscard]] std::optional<int64_t> occurrenceAt(const OccurrenceInput& input);
[[nodiscard]] std::optional<int64_t> firstOccurrenceAfter(const FirstOccurrenceInput& input);
[[nodiscard]] std::optional<int64_t> sameTimeTomorrow(int64_t moment);
