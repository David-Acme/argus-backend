#include "recurrence.hxx"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <ctime>
#include <string>
#include <system_error>

namespace
{
constexpr int kMaxInterval = 999;
constexpr int kMaxMonthDay = 31;
constexpr int64_t kMaxSearch = 1000000;

struct CivilDate
{
  int year{1970};
  int month{1};
  int day{1};
};

struct WallClock
{
  CivilDate date;
  int hour{0};
  int minute{0};
  int second{0};
};

int64_t dayNumberOf(const CivilDate& date)
{
  const int64_t year = date.year - (date.month <= 2 ? 1 : 0);
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yearOfEra = year - era * 400;
  const int64_t month = date.month;
  const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + date.day - 1;
  const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + dayOfEra - 719468;
}

CivilDate civilFromDays(int64_t days)
{
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t dayOfEra = days - era * 146097;
  const int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  const int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const int64_t monthIndex = (5 * dayOfYear + 2) / 153;
  const int64_t day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
  const int64_t month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
  const int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
  return CivilDate{.year = static_cast<int>(year), .month = static_cast<int>(month), .day = static_cast<int>(day)};
}

int daysInMonth(const CivilDate& date)
{
  constexpr std::array<int, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (date.year % 4 == 0 && date.year % 100 != 0) || date.year % 400 == 0;
  return date.month == 2 && leap ? 29 : days[static_cast<std::size_t>(date.month - 1)];
}

std::optional<WallClock> wallOf(int64_t epoch)
{
  const auto moment = static_cast<std::time_t>(epoch);
  std::tm local{};
  if (localtime_r(&moment, &local) == nullptr)
    return std::nullopt;
  return WallClock{.date = CivilDate{.year = local.tm_year + 1900, .month = local.tm_mon + 1, .day = local.tm_mday},
                   .hour = local.tm_hour,
                   .minute = local.tm_min,
                   .second = local.tm_sec};
}

std::optional<int64_t> epochOf(const WallClock& wall)
{
  std::tm local{};
  local.tm_year = wall.date.year - 1900;
  local.tm_mon = wall.date.month - 1;
  local.tm_mday = wall.date.day;
  local.tm_hour = wall.hour;
  local.tm_min = wall.minute;
  local.tm_sec = wall.second;
  local.tm_isdst = -1;
  const std::time_t moment = std::mktime(&local);
  if (moment == static_cast<std::time_t>(-1))
    return std::nullopt;
  return static_cast<int64_t>(moment);
}

std::optional<RecurrenceFrequency> frequencyOf(std::string_view value)
{
  if (value == "DAILY")
    return RecurrenceFrequency::Daily;
  if (value == "WEEKLY")
    return RecurrenceFrequency::Weekly;
  if (value == "MONTHLY")
    return RecurrenceFrequency::Monthly;
  if (value == "YEARLY")
    return RecurrenceFrequency::Yearly;
  return std::nullopt;
}

struct Bounds
{
  int lowest{1};
  int highest{1};
};

std::optional<int> boundedOf(std::string_view value, Bounds bounds)
{
  const std::string digits(value);
  int parsed = 0;
  const char* last = digits.data() + digits.size();
  const auto result = std::from_chars(digits.data(), last, parsed);
  if (result.ec != std::errc{} || result.ptr != last)
    return std::nullopt;
  if (parsed < bounds.lowest || parsed > bounds.highest)
    return std::nullopt;
  return parsed;
}
}

std::optional<RecurrenceRule> parseRecurrenceRule(std::string_view text)
{
  if (text.empty() || text.back() == ';')
    return std::nullopt;
  RecurrenceRule rule;
  bool frequencySeen = false;
  bool intervalSeen = false;
  while (!text.empty()) {
    const std::size_t end = text.find(';');
    const std::string_view part = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    const std::size_t equals = part.find('=');
    if (equals == std::string_view::npos)
      return std::nullopt;
    const std::string_view key = part.substr(0, equals);
    const std::string_view value = part.substr(equals + 1);
    if (key == "FREQ" && !frequencySeen) {
      const auto frequency = frequencyOf(value);
      if (!frequency)
        return std::nullopt;
      rule.frequency = *frequency;
      frequencySeen = true;
    }
    else if (key == "INTERVAL" && !intervalSeen) {
      const auto interval = boundedOf(value, {.lowest = 1, .highest = kMaxInterval});
      if (!interval)
        return std::nullopt;
      rule.interval = *interval;
      intervalSeen = true;
    }
    else if (key == "BYMONTHDAY" && !rule.monthDay) {
      rule.monthDay = boundedOf(value, {.lowest = 1, .highest = kMaxMonthDay});
      if (!rule.monthDay)
        return std::nullopt;
    }
    else {
      return std::nullopt;
    }
  }
  if (!frequencySeen)
    return std::nullopt;
  if (rule.monthDay && rule.frequency != RecurrenceFrequency::Monthly)
    return std::nullopt;
  return rule;
}

std::optional<int64_t> occurrenceAt(const OccurrenceInput& input)
{
  const auto start = wallOf(input.anchor);
  if (!start)
    return std::nullopt;
  const int64_t interval = input.rule.interval;
  const int64_t steps = input.index * interval;
  CivilDate date = start->date;
  switch (input.rule.frequency) {
    case RecurrenceFrequency::Daily:
      date = civilFromDays(dayNumberOf(date) + steps);
      break;
    case RecurrenceFrequency::Weekly:
      date = civilFromDays(dayNumberOf(date) + 7 * steps);
      break;
    case RecurrenceFrequency::Monthly: {
      const int day = input.rule.monthDay.value_or(start->date.day);
      const int64_t lateStart = std::min(day, daysInMonth(start->date)) < start->date.day ? 1 : 0;
      const int64_t monthIndex = date.year * 12LL + (date.month - 1) + lateStart + steps;
      date.year = static_cast<int>(monthIndex / 12);
      date.month = static_cast<int>(monthIndex % 12) + 1;
      date.day = std::min(day, daysInMonth(date));
      break;
    }
    case RecurrenceFrequency::Yearly:
      date.year = start->date.year + static_cast<int>(steps);
      date.day = std::min(start->date.day, daysInMonth(date));
      break;
  }
  return epochOf(WallClock{.date = date, .hour = start->hour, .minute = start->minute, .second = start->second});
}

int64_t seriesStart(const RecurrenceRule& rule, int64_t at)
{
  return occurrenceAt({.rule = rule, .anchor = at, .index = 0}).value_or(at);
}

namespace
{
std::optional<int64_t> searchStart(const FirstOccurrenceInput& input)
{
  const auto start = wallOf(input.anchor);
  const auto now = wallOf(input.moment);
  if (!start || !now)
    return std::nullopt;
  const int64_t interval = input.rule.interval;
  const int64_t daysApart = dayNumberOf(now->date) - dayNumberOf(start->date);
  const int64_t monthsApart = (now->date.year - start->date.year) * 12LL + (now->date.month - start->date.month);
  const int64_t yearsApart = now->date.year - start->date.year;
  int64_t estimate = 0;
  switch (input.rule.frequency) {
    case RecurrenceFrequency::Daily:
      estimate = daysApart / interval;
      break;
    case RecurrenceFrequency::Weekly:
      estimate = daysApart / (7 * interval);
      break;
    case RecurrenceFrequency::Monthly:
      estimate = monthsApart / interval;
      break;
    case RecurrenceFrequency::Yearly:
      estimate = yearsApart / interval;
      break;
  }
  return std::max<int64_t>(estimate - 1, 0);
}
}

std::optional<int64_t> firstOccurrenceAfter(const FirstOccurrenceInput& input)
{
  const auto first = searchStart(input);
  if (!first)
    return std::nullopt;
  int64_t index = *first;
  for (int64_t searched = 0; searched < kMaxSearch; ++searched, ++index) {
    const auto candidate = occurrenceAt({.rule = input.rule, .anchor = input.anchor, .index = index});
    if (!candidate)
      return std::nullopt;
    if (*candidate > input.moment)
      return candidate;
  }
  return std::nullopt;
}

std::optional<int64_t> occurrenceIndex(const FirstOccurrenceInput& input)
{
  if (input.moment < input.anchor)
    return std::nullopt;
  const auto first = searchStart(input);
  if (!first)
    return std::nullopt;
  int64_t index = *first;
  for (int64_t searched = 0; searched < kMaxSearch; ++searched, ++index) {
    const auto candidate = occurrenceAt({.rule = input.rule, .anchor = input.anchor, .index = index});
    if (!candidate || *candidate > input.moment)
      return std::nullopt;
    if (*candidate == input.moment)
      return index;
  }
  return std::nullopt;
}

std::optional<int64_t> sameTimeTomorrow(int64_t moment)
{
  const auto now = wallOf(moment);
  if (!now)
    return std::nullopt;
  return epochOf(WallClock{.date = civilFromDays(dayNumberOf(now->date) + 1),
                           .hour = now->hour,
                           .minute = now->minute,
                           .second = now->second});
}
