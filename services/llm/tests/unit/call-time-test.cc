#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/memory/services/extract/call-time.hxx>

#include <cstdlib>
#include <ctime>
#include <string>

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

int64_t now()
{
  setenv("TZ", "UTC", 1);
  tzset();
  return utc({.day = 7, .hour = 15, .minute = 20});
}

std::optional<int64_t> fireAt(const std::string& text, const std::string& lang = "es")
{
  const auto found = call_time::resolve({.text = text, .lang = lang, .now = now()});
  if (!found)
    return std::nullopt;
  return found->fireAt;
}
}

TEST_CASE("a clock time today, its evening reading, or tomorrow once both passed")
{
  CHECK(fireAt("llámame a las 18:30") == utc({.day = 7, .hour = 18, .minute = 30}));
  CHECK(fireAt("a las nueve") == utc({.day = 7, .hour = 21, .minute = 0}));
  CHECK(fireAt("a las 9 de la noche") == utc({.day = 7, .hour = 21, .minute = 0}));
  CHECK(fireAt("a las cinco y media de la tarde") == utc({.day = 7, .hour = 17, .minute = 30}));
  CHECK(fireAt("a la una") == utc({.day = 8, .hour = 1, .minute = 0}));
  CHECK(fireAt("a las ocho menos cuarto") == utc({.day = 7, .hour = 19, .minute = 45}));
  CHECK(fireAt("call me at 7 pm", "en") == utc({.day = 7, .hour = 19, .minute = 0}));
  CHECK(fireAt("at 9:15", "en") == utc({.day = 7, .hour = 21, .minute = 15}));
  CHECK(fireAt("at 9:15 am", "en") == utc({.day = 8, .hour = 9, .minute = 15}));
  CHECK(fireAt("at noon", "en") == utc({.day = 8, .hour = 12, .minute = 0}));
}

TEST_CASE("a named day with a time")
{
  CHECK(fireAt("recuérdame mañana a las nueve llamar al dentista") ==
        utc({.day = 8, .hour = 9, .minute = 0}));
  CHECK(fireAt("pasado mañana a las 10") == utc({.day = 9, .hour = 10, .minute = 0}));
  CHECK(fireAt("el lunes a las 8 de la mañana") == utc({.day = 12, .hour = 8, .minute = 0}));
  CHECK(fireAt("el miércoles a las 8") == utc({.day = 14, .hour = 8, .minute = 0}));
  CHECK(fireAt("tomorrow at 6:45 am", "en") == utc({.day = 8, .hour = 6, .minute = 45}));
  CHECK_FALSE(fireAt("hoy a las 9 de la mañana"));
}

TEST_CASE("relative times")
{
  CHECK(fireAt("en 20 minutos") == now() + 1200);
  CHECK(fireAt("dentro de una hora") == now() + 3600);
  CHECK(fireAt("en media hora") == now() + 1800);
  CHECK(fireAt("in 2 hours", "en") == now() + 7200);
  CHECK(fireAt("in half an hour", "en") == now() + 1800);
}

TEST_CASE("no time, a vague time or one too far away schedules nothing")
{
  CHECK_FALSE(fireAt("recuérdame que mi cita es el lunes"));
  CHECK_FALSE(fireAt("mañana por la mañana"));
  CHECK_FALSE(fireAt("en casa"));
  CHECK_FALSE(fireAt("a las 25"));
  CHECK_FALSE(fireAt("en 900 horas"));
  CHECK_FALSE(fireAt(""));
}

TEST_CASE("the time phrase is cut out of the topic, accents kept")
{
  const std::string text = "llamar al dentista mañana a las nueve";
  const auto found = call_time::resolve({.text = text, .lang = "es", .now = now()});
  REQUIRE(found);
  CHECK(call_time::withoutPhrase(text, found.value_or(CallTime{})) == "llamar al dentista");
  const std::string first = "mañana a las nueve, llamar a mamá";
  const auto leading = call_time::resolve({.text = first, .lang = "es", .now = now()});
  REQUIRE(leading);
  CHECK(call_time::withoutPhrase(first, leading.value_or(CallTime{})) == "llamar a mamá");
}
