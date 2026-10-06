#include "iso-time.hxx"

#include <charconv>
#include <cstdlib>
#include <ctime>

namespace iso_time
{

namespace
{
constexpr int kSecondsPerMinute = 60;
constexpr int kSecondsPerHour = 3600;

struct Scan
{
  std::string_view text;
  size_t at{0};
};

std::optional<int> number(Scan& scan, size_t digits)
{
  if (scan.at + digits > scan.text.size())
    return std::nullopt;
  int value = 0;
  const auto [end, error] = std::from_chars(scan.text.data() + scan.at, scan.text.data() + scan.at + digits, value);
  if (error != std::errc{} || end != scan.text.data() + scan.at + digits)
    return std::nullopt;
  scan.at += digits;
  return value;
}

bool expect(Scan& scan, char wanted)
{
  if (scan.at >= scan.text.size() || scan.text[scan.at] != wanted)
    return false;
  ++scan.at;
  return true;
}

struct Clock
{
  int hour{0};
  int minute{0};
  int second{0};
};

std::optional<Clock> clockOf(Scan& scan)
{
  const auto hour = number(scan, 2);
  if (!hour || !expect(scan, ':'))
    return std::nullopt;
  const auto minute = number(scan, 2);
  if (!minute)
    return std::nullopt;
  Clock clock{.hour = *hour, .minute = *minute, .second = 0};
  if (scan.at < scan.text.size() && scan.text[scan.at] == ':') {
    ++scan.at;
    const auto second = number(scan, 2);
    if (!second)
      return std::nullopt;
    clock.second = *second;
  }
  if (clock.hour > 23 || clock.minute > 59 || clock.second > 60)
    return std::nullopt;
  return clock;
}

std::optional<int> offsetOf(Scan& scan)
{
  if (scan.at >= scan.text.size())
    return std::nullopt;
  if (scan.text[scan.at] == 'Z' || scan.text[scan.at] == 'z') {
    ++scan.at;
    return 0;
  }
  const char sign = scan.text[scan.at];
  if (sign != '+' && sign != '-')
    return std::nullopt;
  ++scan.at;
  const auto hours = number(scan, 2);
  if (!hours)
    return std::nullopt;
  expect(scan, ':');
  const auto minutes = number(scan, 2);
  if (!minutes || *hours > 14 || *minutes > 59)
    return std::nullopt;
  const int total = *hours * kSecondsPerHour + *minutes * kSecondsPerMinute;
  return sign == '-' ? -total : total;
}

std::string two(int value)
{
  return (value < 10 ? "0" : "") + std::to_string(value);
}
}

std::optional<int64_t> parse(std::string_view text)
{
  Scan scan{.text = text, .at = 0};
  const auto year = number(scan, 4);
  if (!year || !expect(scan, '-'))
    return std::nullopt;
  const auto month = number(scan, 2);
  if (!month || !expect(scan, '-'))
    return std::nullopt;
  const auto day = number(scan, 2);
  if (!day || *month < 1 || *month > 12 || *day < 1 || *day > 31)
    return std::nullopt;
  Clock clock;
  std::optional<int> offset;
  if (scan.at < scan.text.size()) {
    if (!expect(scan, 'T') && !expect(scan, ' '))
      return std::nullopt;
    const auto parsed = clockOf(scan);
    if (!parsed)
      return std::nullopt;
    clock = *parsed;
    if (scan.at < scan.text.size()) {
      offset = offsetOf(scan);
      if (!offset || scan.at != scan.text.size())
        return std::nullopt;
    }
  }
  std::tm at{};
  at.tm_year = *year - 1900;
  at.tm_mon = *month - 1;
  at.tm_mday = *day;
  at.tm_hour = clock.hour;
  at.tm_min = clock.minute;
  at.tm_sec = clock.second;
  at.tm_isdst = -1;
  if (offset)
    return static_cast<int64_t>(timegm(&at)) - *offset;
  return static_cast<int64_t>(mktime(&at));
}

std::string format(int64_t epoch)
{
  const auto seconds = static_cast<std::time_t>(epoch);
  std::tm local{};
  localtime_r(&seconds, &local);
  const long offset = local.tm_gmtoff;
  const long magnitude = offset < 0 ? -offset : offset;
  return std::to_string(local.tm_year + 1900) + "-" + two(local.tm_mon + 1) + "-" + two(local.tm_mday) + "T" +
         two(local.tm_hour) + ":" + two(local.tm_min) + ":" + two(local.tm_sec) + (offset < 0 ? "-" : "+") +
         two(static_cast<int>(magnitude / kSecondsPerHour)) + ":" +
         two(static_cast<int>(magnitude % kSecondsPerHour / kSecondsPerMinute));
}

}
