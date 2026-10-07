#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <text/spoken-time.hxx>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>

namespace
{
struct Moment
{
  int month{10};
  int day{0};
  int hour{0};
  int minute{0};
};

int64_t at(const Moment& moment)
{
  setenv("TZ", "UTC", 1);
  tzset();
  std::tm local{};
  local.tm_year = 2026 - 1900;
  local.tm_mon = moment.month - 1;
  local.tm_mday = moment.day;
  local.tm_hour = moment.hour;
  local.tm_min = moment.minute;
  return static_cast<int64_t>(timegm(&local));
}

struct Case
{
  Moment moment;
  std::string_view lang;
  spoken_time::Day day{spoken_time::Day::Relative};
  bool bare{false};
};

std::string said(const Case& input)
{
  return spoken_time::moment({.epoch = at(input.moment), .now = at({.day = 7, .hour = 15, .minute = 20}), .lang = input.lang, .day = input.day, .bare = input.bare});
}
}

TEST_CASE("a time is spoken as today, tomorrow or the weekday, in the 12-hour clock of each language")
{
  CHECK(said({.moment = {.day = 7, .hour = 15, .minute = 30}, .lang = "es"}) == "hoy a las 3:30 de la tarde");
  CHECK(said({.moment = {.day = 7, .hour = 15, .minute = 30}, .lang = "en"}) == "today at 3:30 PM");
  CHECK(said({.moment = {.day = 8, .hour = 9}, .lang = "es"}) == "mañana a las 9 de la mañana");
  CHECK(said({.moment = {.day = 8, .hour = 9}, .lang = "en"}) == "tomorrow at 9 AM");
  CHECK(said({.moment = {.day = 9, .hour = 13}, .lang = "es"}) == "el viernes 9 a la 1 de la tarde");
  CHECK(said({.moment = {.day = 9, .hour = 13}, .lang = "en"}) == "on Friday the 9th at 1 PM");
  CHECK(said({.moment = {.day = 7, .hour = 21, .minute = 5}, .lang = "es"}) == "hoy a las 9:05 de la noche");
}

TEST_CASE("the read-back names the weekday and the date, even for today and tomorrow")
{
  constexpr auto weekday = spoken_time::Day::Weekday;
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "es", .day = weekday}) == "el jueves 8 a las 3 de la tarde");
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "en", .day = weekday}) == "on Thursday the 8th at 3 PM");
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "en", .day = weekday, .bare = true}) == "Thursday the 8th at 3 PM");
  CHECK(said({.moment = {.day = 7, .hour = 17}, .lang = "es", .day = weekday}) == "el miércoles 7 a las 5 de la tarde");
  CHECK(said({.moment = {.month = 11, .day = 5, .hour = 9}, .lang = "es", .day = weekday}) == "el jueves 5 de noviembre a las 9 de la mañana");
  CHECK(said({.moment = {.month = 11, .day = 5, .hour = 9}, .lang = "en", .day = weekday}) == "on Thursday, November 5th at 9 AM");
}

TEST_CASE("midday, midnight and the small hours are said the way the resolver reads them back")
{
  constexpr auto weekday = spoken_time::Day::Weekday;
  CHECK(said({.moment = {.day = 8, .hour = 12}, .lang = "es", .day = weekday}) == "el jueves 8 a mediodía");
  CHECK(said({.moment = {.day = 8, .hour = 12}, .lang = "en", .day = weekday}) == "on Thursday the 8th at noon");
  CHECK(said({.moment = {.day = 8, .hour = 12, .minute = 30}, .lang = "es", .day = weekday}) == "el jueves 8 a las 12:30 de la tarde");
  CHECK(said({.moment = {.day = 8, .hour = 0}, .lang = "es", .day = weekday}) == "el jueves 8 a las 12 de la madrugada");
  CHECK(said({.moment = {.day = 8, .hour = 0}, .lang = "en", .day = weekday}) == "on Thursday the 8th at 12 AM");
  CHECK(said({.moment = {.day = 8, .hour = 2, .minute = 30}, .lang = "es", .day = weekday}) == "el jueves 8 a las 2:30 de la madrugada");
  CHECK(said({.moment = {.day = 8, .hour = 7}, .lang = "es", .day = weekday}) == "el jueves 8 a las 7 de la mañana");
  CHECK(said({.moment = {.day = 8, .hour = 19}, .lang = "es", .day = weekday}) == "el jueves 8 a las 7 de la noche");
}

TEST_CASE("the ordinal of an English date")
{
  const auto date = [](int day) {
    return spoken_time::weekdayDate({.epoch = at({.month = 10, .day = day}), .now = at({.day = 7}), .lang = "en"});
  };
  CHECK(date(1) == "Thursday the 1st");
  CHECK(date(2) == "Friday the 2nd");
  CHECK(date(3) == "Saturday the 3rd");
  CHECK(date(11) == "Sunday the 11th");
  CHECK(date(12) == "Monday the 12th");
  CHECK(date(13) == "Tuesday the 13th");
  CHECK(date(21) == "Wednesday the 21st");
  CHECK(date(22) == "Thursday the 22nd");
  CHECK(date(23) == "Friday the 23rd");
  CHECK(date(31) == "Saturday the 31st");
  CHECK(spoken_time::weekdayDate({.epoch = at({.day = 12}), .now = at({.day = 7}), .lang = "es"}) == "lunes 12");
}
