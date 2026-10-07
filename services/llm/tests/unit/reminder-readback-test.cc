#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/memory/services/memory/reminder-readback.hxx>

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

struct Said
{
  Moment moment;
  std::string_view lang;
  bool called{false};
};

std::string said(const Said& input)
{
  return reminder_readback::sentence(
      {.fireAt = at(input.moment), .now = at({.day = 7, .hour = 15, .minute = 20}), .lang = input.lang, .called = input.called});
}
}

TEST_CASE("a reminder that will ring says the resolved day and time in words, in Spanish and in English")
{
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "es", .called = true}) == " Te llamaré el jueves 8 a las 3 de la tarde.");
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "en", .called = true}) == " I will call you on Thursday the 8th at 3 PM.");
  CHECK(said({.moment = {.day = 7, .hour = 17, .minute = 30}, .lang = "es", .called = true}) == " Te llamaré el miércoles 7 a las 5:30 de la tarde.");
  CHECK(said({.moment = {.month = 11, .day = 5, .hour = 9}, .lang = "es", .called = true}) ==
        " Te llamaré el jueves 5 de noviembre a las 9 de la mañana.");
  CHECK(said({.moment = {.month = 11, .day = 5, .hour = 9}, .lang = "en", .called = true}) ==
        " I will call you on Thursday, November 5th at 9 AM.");
}

TEST_CASE("a reminder that is only listed says the same day and time without promising a call")
{
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "es"}) == " Quedó en tus recordatorios para el jueves 8 a las 3 de la tarde.");
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "en"}) == " It is in your reminders for Thursday the 8th at 3 PM.");
  CHECK(said({.moment = {.day = 9, .hour = 12}, .lang = "es"}) == " Quedó en tus recordatorios para el viernes 9 a mediodía.");
  CHECK(said({.moment = {.day = 9, .hour = 0}, .lang = "en"}) == " It is in your reminders for Friday the 9th at 12 AM.");
}
