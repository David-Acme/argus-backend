#include <doctest/doctest.h>

#include <feature/conversation/services/conversation-purge.hxx>

namespace
{
std::time_t localAt(int day, int hour, int minute)
{
  std::tm local{};
  local.tm_year = 126;
  local.tm_mon = 9;
  local.tm_mday = day;
  local.tm_hour = hour;
  local.tm_min = minute;
  local.tm_isdst = -1;
  return std::mktime(&local);
}
}

TEST_CASE("the purge waits until the next local 04:30 when it is earlier in the day")
{
  CHECK(secondsUntilDaily(localAt(14, 4, 0), {.hour = 4, .minute = 30}) == 1800.0);
  CHECK(secondsUntilDaily(localAt(14, 0, 0), {.hour = 4, .minute = 30}) == 4.5 * 3600.0);
}

TEST_CASE("after 04:30 the purge waits for the next day's 04:30")
{
  CHECK(secondsUntilDaily(localAt(14, 10, 0), {.hour = 4, .minute = 30}) == 18.5 * 3600.0);
}

TEST_CASE("at exactly 04:30 the purge waits a full day, never a zero delay")
{
  CHECK(secondsUntilDaily(localAt(14, 4, 30), {.hour = 4, .minute = 30}) == 86400.0);
}
