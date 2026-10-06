#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <text/iso-time.hxx>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace
{
struct Moment
{
  int day{0};
  int hour{0};
  int minute{0};
};

int64_t utc(const Moment& moment)
{
  std::tm at{};
  at.tm_year = 2026 - 1900;
  at.tm_mon = 9;
  at.tm_mday = moment.day;
  at.tm_hour = moment.hour;
  at.tm_min = moment.minute;
  return static_cast<int64_t>(timegm(&at));
}

void inUtc()
{
  setenv("TZ", "UTC", 1);
  tzset();
}
}

TEST_CASE("an ISO date-time parses with or without an offset and a second")
{
  inUtc();
  CHECK(iso_time::parse("2026-10-07T15:00") == utc({.day = 7, .hour = 15, .minute = 0}));
  CHECK(iso_time::parse("2026-10-07T15:00:30") == utc({.day = 7, .hour = 15, .minute = 0}) + 30);
  CHECK(iso_time::parse("2026-10-07 15:00") == utc({.day = 7, .hour = 15, .minute = 0}));
  CHECK(iso_time::parse("2026-10-07T15:00:00Z") == utc({.day = 7, .hour = 15, .minute = 0}));
  CHECK(iso_time::parse("2026-10-07T15:00:00-05:00") == utc({.day = 7, .hour = 20, .minute = 0}));
  CHECK(iso_time::parse("2026-10-07T15:00:00+05:30") == utc({.day = 7, .hour = 9, .minute = 30}));
  CHECK(iso_time::parse("2026-10-07") == utc({.day = 7, .hour = 0, .minute = 0}));
}

TEST_CASE("a time without an offset is read in the local zone")
{
  setenv("TZ", "PET5", 1);
  tzset();
  CHECK(iso_time::parse("2026-10-07T15:00") == utc({.day = 7, .hour = 20, .minute = 0}));
  CHECK(iso_time::format(utc({.day = 7, .hour = 20, .minute = 0})) == "2026-10-07T15:00:00-05:00");
  inUtc();
}

TEST_CASE("text that is not an ISO date-time does not parse")
{
  const std::vector<std::string> refused{"", "mañana", "2026-13-01T10:00", "2026-10-32", "2026-10-07T25:00",
                                         "2026-10-07T10:00:00X", "2026-10-07T10:00:00+99:00", "10/07/2026",
                                         "2026-10-07T10"};
  for (const auto& text : refused) {
    INFO(text);
    CHECK_FALSE(iso_time::parse(text).has_value());
  }
}

TEST_CASE("an epoch is written back as local ISO time with its offset and reads back the same")
{
  inUtc();
  CHECK(iso_time::format(utc({.day = 7, .hour = 15, .minute = 0})) == "2026-10-07T15:00:00+00:00");
  CHECK(iso_time::parse(iso_time::format(utc({.day = 8, .hour = 9, .minute = 5}))) == utc({.day = 8, .hour = 9, .minute = 5}));
}
