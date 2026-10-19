#include <doctest/doctest.h>

#include <feature/memory/services/extract/call-time.hxx>
#include <feature/memory/services/extract/repeat-rule.hxx>

#include "support/require-value.hxx"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace
{
struct Moment
{
  int year{2026};
  int month{10};
  int day{0};
  int hour{0};
  int minute{0};
};

int64_t utc(const Moment& moment)
{
  setenv("TZ", "UTC", 1);
  tzset();
  std::tm at{};
  at.tm_year = moment.year - 1900;
  at.tm_mon = moment.month - 1;
  at.tm_mday = moment.day;
  at.tm_hour = moment.hour;
  at.tm_min = moment.minute;
  return static_cast<int64_t>(timegm(&at));
}

int64_t wednesdayAfternoon()
{
  return utc({.year = 2026, .month = 10, .day = 7, .hour = 15, .minute = 20});
}

struct Heard
{
  std::string_view text;
  std::string_view lang;
  std::string_view rule;
};

std::string ruleOf(std::string_view text, std::string_view lang)
{
  const RepeatReading reading = repeat_rule::read({.text = text, .lang = lang});
  return reading.repeat ? repeat_rule::format(*reading.repeat) : std::string("-");
}

constexpr std::array<Heard, 22> kSpanish{{
    {.text = "recuérdame cada día a las 9 tomar las pastillas", .lang = "es", .rule = "FREQ=DAILY"},
    {.text = "recuérdame todos los días a las 9 tomar las pastillas", .lang = "es", .rule = "FREQ=DAILY"},
    {.text = "recuérdame tomar las pastillas a diario a las 9", .lang = "es", .rule = "FREQ=DAILY"},
    {.text = "recuérdame tomar las pastillas diario a las 9", .lang = "es", .rule = "FREQ=DAILY"},
    {.text = "recuérdame todas las noches a las 9 cerrar la puerta", .lang = "es", .rule = "FREQ=DAILY"},
    {.text = "recuérdame cada lunes a las 8 sacar la basura", .lang = "es", .rule = "FREQ=WEEKLY;BYDAY=MO"},
    {.text = "Recuérdame todos los lunes a las 8 sacar la basura", .lang = "es", .rule = "FREQ=WEEKLY;BYDAY=MO"},
    {.text = "recuérdame los lunes y miércoles a las 7 ir al gimnasio", .lang = "es", .rule = "FREQ=WEEKLY;BYDAY=MO,WE"},
    {.text = "recuérdame los martes, jueves y sábados a las 6 regar", .lang = "es", .rule = "FREQ=WEEKLY;BYDAY=TU,TH,SA"},
    {.text = "recuérdame cada semana a las 10 llamar a mamá", .lang = "es", .rule = "FREQ=WEEKLY"},
    {.text = "recuérdame cada dos semanas los viernes a las 6 limpiar", .lang = "es", .rule = "FREQ=WEEKLY;INTERVAL=2;BYDAY=FR"},
    {.text = "recuérdame entre semana a las 7 sacar al perro", .lang = "es", .rule = "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR"},
    {.text = "recuérdame de lunes a viernes a las 7 sacar al perro", .lang = "es", .rule = "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR"},
    {.text = "recuérdame los días útiles a las 7 marcar asistencia", .lang = "es", .rule = "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR"},
    {.text = "recuérdame los fines de semana a las 10 lavar el carro", .lang = "es", .rule = "FREQ=WEEKLY;BYDAY=SU,SA"},
    {.text = "recuérdame cada mes a las 9 pagar la luz", .lang = "es", .rule = "FREQ=MONTHLY"},
    {.text = "recuérdame el día 5 de cada mes a las 9 pagar el alquiler", .lang = "es", .rule = "FREQ=MONTHLY;BYMONTHDAY=5"},
    {.text = "recuérdame el 15 de cada mes pagar la tarjeta a las 10", .lang = "es", .rule = "FREQ=MONTHLY;BYMONTHDAY=15"},
    {.text = "recuérdame una vez al mes revisar el extintor a las 10", .lang = "es", .rule = "FREQ=MONTHLY"},
    {.text = "recuérdame cada 2 días a las 8 regar las plantas", .lang = "es", .rule = "FREQ=DAILY;INTERVAL=2"},
    {.text = "recuérdame interdiario a las 8 tomar la vitamina", .lang = "es", .rule = "FREQ=DAILY;INTERVAL=2"},
    {.text = "recuérdame día por medio regar el jardín a las 7", .lang = "es", .rule = "FREQ=DAILY;INTERVAL=2"},
}};

constexpr std::array<Heard, 11> kEnglish{{
    {.text = "remind me every day at 9 to take my pills", .lang = "en", .rule = "FREQ=DAILY"},
    {.text = "remind me daily at 9 to take my pills", .lang = "en", .rule = "FREQ=DAILY"},
    {.text = "remind me every night at 9 to lock the door", .lang = "en", .rule = "FREQ=DAILY"},
    {.text = "remind me every other day at 8 to water the plants", .lang = "en", .rule = "FREQ=DAILY;INTERVAL=2"},
    {.text = "remind me every Monday at 8 to take out the trash", .lang = "en", .rule = "FREQ=WEEKLY;BYDAY=MO"},
    {.text = "remind me on Mondays and Wednesdays at 7 to go to the gym", .lang = "en", .rule = "FREQ=WEEKLY;BYDAY=MO,WE"},
    {.text = "remind me every weekday at 7 to walk the dog", .lang = "en", .rule = "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR"},
    {.text = "remind me Monday through Friday at 7 to walk the dog", .lang = "en", .rule = "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR"},
    {.text = "remind me every 2 weeks at 10 to call mom", .lang = "en", .rule = "FREQ=WEEKLY;INTERVAL=2"},
    {.text = "remind me every month at 9 to pay the rent", .lang = "en", .rule = "FREQ=MONTHLY"},
    {.text = "remind me on the 5th of every month at 9 to pay the rent", .lang = "en", .rule = "FREQ=MONTHLY;BYMONTHDAY=5"},
}};

constexpr std::array<Heard, 11> kUnreadable{{
    {.text = "recuérdame cada hora tomar agua", .lang = "es", .rule = "-"},
    {.text = "recuérdame cada 2 horas tomar agua", .lang = "es", .rule = "-"},
    {.text = "recuérdame dos veces al día tomar la pastilla", .lang = "es", .rule = "-"},
    {.text = "recuérdame todos los días excepto el domingo a las 8 caminar", .lang = "es", .rule = "-"},
    {.text = "recuérdame cada año a las 9 revisar el seguro", .lang = "es", .rule = "-"},
    {.text = "recuérdame de vez en cuando estirar", .lang = "es", .rule = "-"},
    {.text = "remind me every hour to drink water", .lang = "en", .rule = "-"},
    {.text = "remind me twice a day to take the pill", .lang = "en", .rule = "-"},
    {.text = "remind me from time to time to stretch", .lang = "en", .rule = "-"},
    {.text = "recuérdame el primer lunes de cada mes a las 9 pagar la cuota", .lang = "es", .rule = "-"},
    {.text = "remind me on the first Monday of every month at 9 to pay the fee", .lang = "en", .rule = "-"},
}};

constexpr std::array<Heard, 10> kOneOff{{
    {.text = "avísame cada vez que llegue Juan", .lang = "es", .rule = "-"},
    {.text = "recuérdame el lunes a las 8 sacar la basura", .lang = "es", .rule = "-"},
    {.text = "recuérdame mañana a las 9 llamar al dentista", .lang = "es", .rule = "-"},
    {.text = "recuérdame comprar el diario a las 8", .lang = "es", .rule = "-"},
    {.text = "recuérdame que cada uno traiga su plato", .lang = "es", .rule = "-"},
    {.text = "recuérdame este fin de semana lavar el carro", .lang = "es", .rule = "-"},
    {.text = "remind me on Monday at 8 to call the bank", .lang = "en", .rule = "-"},
    {.text = "let me know every time the door opens", .lang = "en", .rule = "-"},
    {.text = "remind me to send the weekly report at 9", .lang = "en", .rule = "-"},
    {.text = "remind me tomorrow at 9 to call the dentist", .lang = "en", .rule = "-"},
}};
}

TEST_CASE("a Spanish recurrence, Peruvian forms included, becomes the rule the reminder carries")
{
  for (const Heard& heard : kSpanish) {
    INFO(heard.text);
    CHECK(ruleOf(heard.text, heard.lang) == heard.rule);
    CHECK(repeat_rule::read({.text = heard.text, .lang = heard.lang}).mentioned);
  }
}

TEST_CASE("an English recurrence becomes the same rule")
{
  for (const Heard& heard : kEnglish) {
    INFO(heard.text);
    CHECK(ruleOf(heard.text, heard.lang) == heard.rule);
  }
}

TEST_CASE("a recurrence the reminders cannot keep is heard as one and never read as a one-off")
{
  for (const Heard& heard : kUnreadable) {
    INFO(heard.text);
    const RepeatReading reading = repeat_rule::read({.text = heard.text, .lang = heard.lang});
    CHECK(reading.mentioned);
    CHECK_FALSE(reading.repeat.has_value());
  }
}

TEST_CASE("every time, each one, a single weekday and a newspaper are not recurrences")
{
  for (const Heard& heard : kOneOff) {
    INFO(heard.text);
    const RepeatReading reading = repeat_rule::read({.text = heard.text, .lang = heard.lang});
    CHECK_FALSE(reading.mentioned);
    CHECK_FALSE(reading.repeat.has_value());
  }
}

TEST_CASE("the part of the day a recurrence names reads a bare hour, and the phrase is where it was said")
{
  const std::string text = "recuérdame todas las noches a las 9 cerrar la puerta";
  const RepeatReading reading = repeat_rule::read({.text = text, .lang = "es"});
  CHECK(reading.part == DayPart::Night);
  CHECK(text.substr(reading.phraseBegin, reading.phraseEnd - reading.phraseBegin) == "todas las noches");
  const auto clock = requireValue(call_time::clockOf({.text = text, .part = reading.part}));
  CHECK(clock.minuteOfDay == 21 * 60);
  CHECK(text.substr(clock.phraseBegin, clock.phraseEnd - clock.phraseBegin) == "a las 9");

  CHECK(requireValue(call_time::clockOf({.text = "cada día a las 8", .part = DayPart::None})).minuteOfDay == 8 * 60);
  CHECK(requireValue(call_time::clockOf({.text = "cada lunes a las 8 menos cuarto", .part = DayPart::None})).minuteOfDay ==
        7 * 60 + 45);
  CHECK(requireValue(call_time::clockOf({.text = "every day at 9:30 pm", .part = DayPart::None})).minuteOfDay == 21 * 60 + 30);
  CHECK(requireValue(call_time::clockOf({.text = "cada día a mediodía", .part = DayPart::None})).minuteOfDay == 12 * 60);
  CHECK_FALSE(call_time::clockOf({.text = "cada lunes sacar la basura", .part = DayPart::None}).has_value());

  const std::string monthly = "recuérdame el día 5 de cada mes a las 9 pagar el alquiler";
  const RepeatReading inMonth = repeat_rule::read({.text = monthly, .lang = "es"});
  CHECK(monthly.substr(inMonth.phraseBegin, inMonth.phraseEnd - inMonth.phraseBegin) == "el día 5 de cada mes");
}

TEST_CASE("the argument grammar round-trips and the productivity rule keeps only frequency and interval")
{
  for (const std::string_view rule : {"FREQ=DAILY", "FREQ=DAILY;INTERVAL=2", "FREQ=WEEKLY;BYDAY=MO", "FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE,FR",
                                      "FREQ=MONTHLY", "FREQ=MONTHLY;BYMONTHDAY=31", "FREQ=YEARLY"}) {
    INFO(rule);
    CHECK(repeat_rule::format(requireValue(repeat_rule::parse(rule))) == rule);
  }
  CHECK(repeat_rule::format(requireValue(repeat_rule::parse("BYDAY=WE,MO;FREQ=WEEKLY"))) == "FREQ=WEEKLY;BYDAY=MO,WE");
  for (const std::string_view rule : {"", "FREQ=HOURLY", "INTERVAL=2", "FREQ=WEEKLY;", "FREQ=DAILY;BYDAY=MO",
                                      "FREQ=WEEKLY;BYDAY=XX", "FREQ=WEEKLY;BYDAY=MO,MO", "FREQ=MONTHLY;BYMONTHDAY=32",
                                      "FREQ=WEEKLY;INTERVAL=1000", "FREQ=WEEKLY;INTERVAL=0", "FREQ=DAILY;FREQ=DAILY",
                                      "FREQ=WEEKLY;BYMONTHDAY=5"}) {
    INFO(rule);
    CHECK_FALSE(repeat_rule::parse(rule).has_value());
  }
  CHECK(repeat_rule::productivityRule(requireValue(repeat_rule::parse("FREQ=WEEKLY;INTERVAL=2;BYDAY=FR"))) == "FREQ=WEEKLY;INTERVAL=2");
  CHECK(repeat_rule::productivityRule(requireValue(repeat_rule::parse("FREQ=MONTHLY;BYMONTHDAY=5"))) == "FREQ=MONTHLY");
  CHECK(repeat_rule::productivityRule(requireValue(repeat_rule::parse("FREQ=DAILY"))) == "FREQ=DAILY");
}

TEST_CASE("the first occurrence is the next matching instant from now, one per named weekday")
{
  const int64_t now = wednesdayAfternoon();
  const auto firsts = [now](std::string_view rule, int minuteOfDay) {
    const Repeat repeat = requireValue(repeat_rule::parse(rule));
    return repeat_rule::firstOccurrences({.repeat = repeat, .minuteOfDay = minuteOfDay, .now = now});
  };
  CHECK(firsts("FREQ=WEEKLY;BYDAY=MO", 8 * 60) == std::vector<int64_t>{utc({.day = 12, .hour = 8})});
  CHECK(firsts("FREQ=WEEKLY;BYDAY=MO,WE", 7 * 60) ==
        std::vector<int64_t>{utc({.day = 12, .hour = 7}), utc({.day = 14, .hour = 7})});
  CHECK(firsts("FREQ=WEEKLY;BYDAY=WE", 18 * 60) == std::vector<int64_t>{utc({.day = 7, .hour = 18})});
  CHECK(firsts("FREQ=DAILY", 9 * 60) == std::vector<int64_t>{utc({.day = 8, .hour = 9})});
  CHECK(firsts("FREQ=DAILY", 18 * 60) == std::vector<int64_t>{utc({.day = 7, .hour = 18})});
  CHECK(firsts("FREQ=WEEKLY", 16 * 60) == std::vector<int64_t>{utc({.day = 7, .hour = 16})});
  CHECK(firsts("FREQ=MONTHLY;BYMONTHDAY=5", 9 * 60) == std::vector<int64_t>{utc({.month = 11, .day = 5, .hour = 9})});
  CHECK(firsts("FREQ=MONTHLY;BYMONTHDAY=7", 16 * 60) == std::vector<int64_t>{utc({.day = 7, .hour = 16})});
  CHECK(firsts("FREQ=MONTHLY;BYMONTHDAY=31", 9 * 60) == std::vector<int64_t>{utc({.day = 31, .hour = 9})});
  CHECK(firsts("FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR", 7 * 60).size() == 5);

  const Repeat endOfMonth = requireValue(repeat_rule::parse("FREQ=MONTHLY;BYMONTHDAY=31"));
  CHECK(repeat_rule::firstOccurrences({.repeat = endOfMonth, .minuteOfDay = 9 * 60, .now = utc({.month = 11, .day = 5, .hour = 10})}) ==
        std::vector<int64_t>{utc({.month = 12, .day = 31, .hour = 9})});
}
