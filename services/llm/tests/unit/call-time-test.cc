#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/memory/services/extract/call-time.hxx>
#include <text/spoken-time.hxx>

#include <array>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>

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
  std::tm at{};
  at.tm_year = moment.year - 1900;
  at.tm_mon = moment.month - 1;
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

struct Conflict
{
  int64_t byWeekday{0};
  int64_t byDate{0};

  bool operator==(const Conflict&) const = default;
};

std::optional<Conflict> conflictOf(const std::string& text, const std::string& lang = "es")
{
  const auto found = call_time::read({.text = text, .lang = lang, .now = now()}).conflict;
  if (!found)
    return std::nullopt;
  return Conflict{.byWeekday = found->byWeekday, .byDate = found->byDate};
}

constexpr Moment today(int hour, int minute = 0)
{
  return {.day = 7, .hour = hour, .minute = minute};
}

constexpr Moment tomorrow(int hour, int minute = 0)
{
  return {.day = 8, .hour = hour, .minute = minute};
}
}

TEST_CASE("a bare hour from 1 to 6 is the afternoon, 7 to 11 the morning, 12 noon, and the next such time is taken")
{
  CHECK(fireAt("a las cinco") == utc(today(17)));
  CHECK(fireAt("a las 5") == utc(today(17)));
  CHECK(fireAt("para las cinco") == utc(today(17)));
  CHECK(fireAt("a las seis") == utc(today(18)));
  CHECK(fireAt("a las tres") == utc(tomorrow(15)));
  CHECK(fireAt("a las dos") == utc(tomorrow(14)));
  CHECK(fireAt("a la una") == utc(tomorrow(13)));
  CHECK(fireAt("a las siete") == utc(today(19)));
  CHECK(fireAt("a las nueve") == utc(today(21)));
  CHECK(fireAt("a las diez") == utc(today(22)));
  CHECK(fireAt("a las once") == utc(today(23)));
  CHECK(fireAt("a las doce") == utc(tomorrow(12)));
  CHECK(fireAt("at five", "en") == utc(today(17)));
  CHECK(fireAt("at 2", "en") == utc(tomorrow(14)));
  CHECK(fireAt("at 9", "en") == utc(today(21)));
  CHECK(fireAt("at 12", "en") == utc(tomorrow(12)));
  CHECK(fireAt("at 5 o'clock", "en") == utc(today(17)));
  CHECK(fireAt("at 3 o'clock", "en") == utc(tomorrow(15)));
}

TEST_CASE("an explicit qualifier always wins over the bare-hour reading")
{
  CHECK(fireAt("a las 5 de la mañana") == utc(tomorrow(5)));
  CHECK(fireAt("a las 5 en la mañana") == utc(tomorrow(5)));
  CHECK(fireAt("a las 5 por la mañana") == utc(tomorrow(5)));
  CHECK(fireAt("a las 5 de la madrugada") == utc(tomorrow(5)));
  CHECK(fireAt("a las 2 de la madrugada") == utc(tomorrow(2)));
  CHECK(fireAt("a las 5 de la tarde") == utc(today(17)));
  CHECK(fireAt("a las 5 en la tarde") == utc(today(17)));
  CHECK(fireAt("a las 4 de la tarde") == utc(today(16)));
  CHECK(fireAt("a las 7 de la tarde") == utc(today(19)));
  CHECK(fireAt("a las 3 de la tarde") == utc(tomorrow(15)));
  CHECK(fireAt("a la una de la tarde") == utc(tomorrow(13)));
  CHECK(fireAt("a las 9 de la noche") == utc(today(21)));
  CHECK(fireAt("a las 8 de la noche") == utc(today(20)));
  CHECK(fireAt("a las 11 de la noche") == utc(today(23)));
  CHECK(fireAt("a las 2 de la noche") == utc(tomorrow(2)));
  CHECK(fireAt("a las 9 de la mañana") == utc(tomorrow(9)));
  CHECK(fireAt("a las 12 de la noche") == utc(tomorrow(0)));
  CHECK(fireAt("a la una del mediodía") == utc(tomorrow(13)));
  CHECK(fireAt("a las doce del mediodía") == utc(tomorrow(12)));
  CHECK(fireAt("at 4 in the afternoon", "en") == utc(today(16)));
  CHECK(fireAt("at 4 in the evening", "en") == utc(today(16)));
  CHECK(fireAt("at 7 in the evening", "en") == utc(today(19)));
  CHECK(fireAt("at 4 in the morning", "en") == utc(tomorrow(4)));
  CHECK(fireAt("at 9 at night", "en") == utc(today(21)));
  CHECK(fireAt("at 2 at night", "en") == utc(tomorrow(2)));
}

TEST_CASE("a. m. and p. m. in every written form")
{
  for (const char* form : {"a las 4 p. m.", "a las 4 p.m.", "a las 4 p.m", "a las 4 pm", "a las 4 PM", "a las 4pm", "a las 4 P. M.",
                           "a las 4 P.M.", "a las 4:00 p. m.", "a las cuatro p. m.", "a las cuatro pm"})
    CHECK_MESSAGE(fireAt(form) == utc(today(16)), form);
  for (const char* form : {"a las 4 a. m.", "a las 4 a.m.", "a las 4 a.m", "a las 4 am", "a las 4 AM", "a las 4am", "a las 4 A. M.",
                           "a las 4 A.M.", "a las 4:00 a. m.", "a las cuatro a. m.", "a las cuatro am"})
    CHECK_MESSAGE(fireAt(form) == utc(tomorrow(4)), form);
  for (const char* form : {"at 4 pm", "at 4 p.m.", "at 4 p. m.", "at 4PM", "at 4pm", "at 4 P.M.", "at 4:00 pm", "at four pm"})
    CHECK_MESSAGE(fireAt(form, "en") == utc(today(16)), form);
  for (const char* form : {"at 4 am", "at 4 a.m.", "at 4 a. m.", "at 4AM", "at 4am", "at 4 A.M.", "at 4:00 am", "at four am"})
    CHECK_MESSAGE(fireAt(form, "en") == utc(tomorrow(4)), form);
  CHECK(fireAt("at 9 am", "en") == utc(tomorrow(9)));
  CHECK(fireAt("at 3 pm", "en") == utc(tomorrow(15)));
  CHECK(fireAt("call me at 7 pm", "en") == utc(today(19)));
  CHECK(fireAt("tomorrow 5pm", "en") == utc(tomorrow(17)));
  CHECK(fireAt("mañana 5 pm") == utc(tomorrow(17)));
  CHECK(fireAt("a las 12 a. m.") == utc(tomorrow(0)));
  CHECK(fireAt("a las 12 am") == utc(tomorrow(0)));
  CHECK(fireAt("a las 12 p. m.") == utc(tomorrow(12)));
  CHECK(fireAt("a las 12 pm") == utc(tomorrow(12)));
  CHECK(fireAt("at 12 am", "en") == utc(tomorrow(0)));
  CHECK(fireAt("at 12 pm", "en") == utc(tomorrow(12)));
}

TEST_CASE("midday and midnight")
{
  CHECK(fireAt("a mediodía") == utc(tomorrow(12)));
  CHECK(fireAt("al mediodía") == utc(tomorrow(12)));
  CHECK(fireAt("mañana a mediodía") == utc(tomorrow(12)));
  CHECK(fireAt("el viernes a mediodía") == utc({.day = 9, .hour = 12}));
  CHECK(fireAt("at noon", "en") == utc(tomorrow(12)));
  CHECK(fireAt("at midday", "en") == utc(tomorrow(12)));
  CHECK(fireAt("a medianoche") == utc(tomorrow(0)));
  CHECK(fireAt("esta noche a medianoche") == utc(tomorrow(0)));
  CHECK(fireAt("at midnight", "en") == utc(tomorrow(0)));
  CHECK(fireAt("tonight at midnight", "en") == utc(tomorrow(0)));
  CHECK(fireAt("mañana a medianoche") == utc({.day = 9, .hour = 0}));
  CHECK(fireAt("el viernes a medianoche") == utc({.day = 10, .hour = 0}));
}

TEST_CASE("minutes keep their value in every spelling")
{
  CHECK(fireAt("a las cinco y media") == utc(today(17, 30)));
  CHECK(fireAt("a las 5 y cuarto") == utc(today(17, 15)));
  CHECK(fireAt("a las 6 menos cuarto") == utc(today(17, 45)));
  CHECK(fireAt("a las cinco y diez") == utc(today(17, 10)));
  CHECK(fireAt("a las 5 menos diez") == utc(today(16, 50)));
  CHECK(fireAt("a las 5 y 20") == utc(today(17, 20)));
  CHECK(fireAt("a las seis y media") == utc(today(18, 30)));
  CHECK(fireAt("a las 8 y media de la noche") == utc(today(20, 30)));
  CHECK(fireAt("a las 10 y media de la noche") == utc(today(22, 30)));
  CHECK(fireAt("a las cinco y media de la tarde") == utc(today(17, 30)));
  CHECK(fireAt("a las 9 y cuarto de la mañana") == utc(tomorrow(9, 15)));
  CHECK(fireAt("a las 9 y media") == utc(today(21, 30)));
  CHECK(fireAt("a las ocho menos cuarto") == utc(today(19, 45)));
  CHECK(fireAt("a las cinco en punto") == utc(today(17)));
  CHECK(fireAt("a las 5:30") == utc(today(17, 30)));
  CHECK(fireAt("a las 5:30 pm") == utc(today(17, 30)));
  CHECK(fireAt("a las 5:45 de la tarde") == utc(today(17, 45)));
  CHECK(fireAt("a las 5:30 de la mañana") == utc(tomorrow(5, 30)));
  CHECK(fireAt("a las 7:45") == utc(today(19, 45)));
  CHECK(fireAt("a las 7:05 pm") == utc(today(19, 5)));
  CHECK(fireAt("a las 11:15") == utc(today(23, 15)));
  CHECK(fireAt("a las 11:15 pm") == utc(today(23, 15)));
  CHECK(fireAt("a las 12:30") == utc(tomorrow(12, 30)));
  CHECK(fireAt("at eleven fifteen", "en") == utc(today(23, 15)));
  CHECK(fireAt("at eleven fifteen pm", "en") == utc(today(23, 15)));
  CHECK(fireAt("at five thirty", "en") == utc(today(17, 30)));
  CHECK(fireAt("at five forty five", "en") == utc(today(17, 45)));
  CHECK(fireAt("at five oh five", "en") == utc(today(17, 5)));
  CHECK(fireAt("at nine hundred p. m.", "en") == utc(today(21)));
  CHECK(fireAt("on march nineteen at one pm", "en") == utc({.year = 2027, .month = 3, .day = 19, .hour = 13}));
  CHECK(fireAt("at half past five", "en") == utc(today(17, 30)));
  CHECK(fireAt("call me half past five", "en") == utc(today(17, 30)));
  CHECK(fireAt("at half past five pm", "en") == utc(today(17, 30)));
  CHECK(fireAt("at half past eleven", "en") == utc(today(23, 30)));
  CHECK(fireAt("at quarter to six", "en") == utc(today(17, 45)));
  CHECK(fireAt("at quarter past nine", "en") == utc(today(21, 15)));
  CHECK(fireAt("at ten past five", "en") == utc(today(17, 10)));
  CHECK(fireAt("at twenty five to six", "en") == utc(today(17, 35)));
  CHECK(fireAt("at 5:30", "en") == utc(today(17, 30)));
  CHECK(fireAt("at 5:30 pm", "en") == utc(today(17, 30)));
  CHECK(fireAt("at 11:15 am", "en") == utc(tomorrow(11, 15)));
  CHECK(fireAt("at 9:15", "en") == utc(today(21, 15)));
}

TEST_CASE("a 24-hour time is taken as written")
{
  CHECK(fireAt("llámame a las 18:30") == utc(today(18, 30)));
  CHECK(fireAt("a las 21:05") == utc(today(21, 5)));
  CHECK(fireAt("a las 15:30") == utc(today(15, 30)));
  CHECK(fireAt("a las 15:00") == utc(tomorrow(15)));
  CHECK(fireAt("a las 13") == utc(tomorrow(13)));
  CHECK(fireAt("a las 00:30") == utc(tomorrow(0, 30)));
  CHECK(fireAt("a las 07:30") == utc(tomorrow(7, 30)));
  CHECK(fireAt("a las 05:30") == utc(tomorrow(5, 30)));
}

TEST_CASE("tomorrow, the day after and a part of the day")
{
  CHECK(fireAt("recuérdame mañana a las nueve llamar al dentista") == utc(tomorrow(9)));
  CHECK(fireAt("mañana a las cinco") == utc(tomorrow(17)));
  CHECK(fireAt("mañana a las 6") == utc(tomorrow(18)));
  CHECK(fireAt("mañana a las 7") == utc(tomorrow(7)));
  CHECK(fireAt("mañana a las 9 de la noche") == utc(tomorrow(21)));
  CHECK(fireAt("pasado mañana a las 10") == utc({.day = 9, .hour = 10}));
  CHECK(fireAt("pasado mañana a las 3") == utc({.day = 9, .hour = 15}));
  CHECK(fireAt("mañana por la mañana a las 8") == utc(tomorrow(8)));
  CHECK(fireAt("mañana en la mañana a las 8") == utc(tomorrow(8)));
  CHECK(fireAt("mañana por la tarde a las 5") == utc(tomorrow(17)));
  CHECK(fireAt("mañana en la tarde a las 5") == utc(tomorrow(17)));
  CHECK(fireAt("mañana por la noche a las 10") == utc(tomorrow(22)));
  CHECK(fireAt("mañana en la noche a las 9") == utc(tomorrow(21)));
  CHECK(fireAt("mañana a las 8 en la noche") == utc(tomorrow(20)));
  CHECK(fireAt("mañana a las 5 en la mañana") == utc(tomorrow(5)));
  CHECK(fireAt("tomorrow at 5", "en") == utc(tomorrow(17)));
  CHECK(fireAt("tomorrow at 9", "en") == utc(tomorrow(9)));
  CHECK(fireAt("tomorrow at 6:45 am", "en") == utc(tomorrow(6, 45)));
  CHECK(fireAt("tomorrow at 5 in the morning", "en") == utc(tomorrow(5)));
  CHECK(fireAt("tomorrow morning at 8", "en") == utc(tomorrow(8)));
  CHECK(fireAt("tomorrow afternoon at 3", "en") == utc(tomorrow(15)));
  CHECK(fireAt("tomorrow evening at 8", "en") == utc(tomorrow(20)));
  CHECK(fireAt("tomorrow night at 9", "en") == utc(tomorrow(21)));
  CHECK(fireAt("the day after tomorrow at 3", "en") == utc({.day = 9, .hour = 15}));
}

TEST_CASE("today names a time that is still ahead, never a past one and never tomorrow")
{
  CHECK(fireAt("hoy a las cinco") == utc(today(17)));
  CHECK(fireAt("hoy a las 4") == utc(today(16)));
  CHECK(fireAt("hoy a las nueve") == utc(today(21)));
  CHECK_FALSE(fireAt("hoy a las 9 de la mañana"));
  CHECK_FALSE(fireAt("esta mañana a las 9"));
  CHECK(fireAt("esta tarde a las 5") == utc(today(17)));
  CHECK(fireAt("esta tarde a las 4") == utc(today(16)));
  CHECK(fireAt("esta noche a las 9") == utc(today(21)));
  CHECK(fireAt("esta noche a las 11") == utc(today(23)));
  CHECK(fireAt("esta noche a las 12") == utc(tomorrow(0)));
  CHECK(fireAt("today at 5", "en") == utc(today(17)));
  CHECK_FALSE(fireAt("today at 2", "en"));
  CHECK(fireAt("tonight at 9", "en") == utc(today(21)));
  CHECK(fireAt("this afternoon at 4", "en") == utc(today(16)));
  CHECK(fireAt("this evening at 7", "en") == utc(today(19)));
  CHECK_FALSE(fireAt("this morning at 9", "en"));
}

TEST_CASE("a weekday is the next one strictly after today, unless today is said")
{
  CHECK(fireAt("el viernes a las cinco") == utc({.day = 9, .hour = 17}));
  CHECK(fireAt("el viernes a las 5 de la mañana") == utc({.day = 9, .hour = 5}));
  CHECK(fireAt("el viernes a las 2") == utc({.day = 9, .hour = 14}));
  CHECK(fireAt("el sábado a las 2") == utc({.day = 10, .hour = 14}));
  CHECK(fireAt("el domingo a las 11") == utc({.day = 11, .hour = 11}));
  CHECK(fireAt("el jueves a las 12") == utc({.day = 8, .hour = 12}));
  CHECK(fireAt("el lunes a las 8 de la mañana") == utc({.day = 12, .hour = 8}));
  CHECK(fireAt("el lunes a las 8") == utc({.day = 12, .hour = 8}));
  CHECK(fireAt("el martes a las 6 y media") == utc({.day = 13, .hour = 18, .minute = 30}));
  CHECK(fireAt("el miércoles a las 8") == utc({.day = 14, .hour = 8}));
  CHECK(fireAt("el miércoles a las 5") == utc({.day = 14, .hour = 17}));
  CHECK(fireAt("miércoles a las 5") == utc({.day = 14, .hour = 17}));
  CHECK(fireAt("hoy miércoles a las 5") == utc(today(17)));
  CHECK(fireAt("on friday at 5", "en") == utc({.day = 9, .hour = 17}));
  CHECK(fireAt("friday at 5 pm", "en") == utc({.day = 9, .hour = 17}));
  CHECK(fireAt("on Monday at 9 am", "en") == utc({.day = 12, .hour = 9}));
  CHECK(fireAt("on Wednesday at 10", "en") == utc({.day = 14, .hour = 10}));
  CHECK(fireAt("next friday at 4", "en") == utc({.day = 9, .hour = 16}));
}

TEST_CASE("a day of the month with no month is the next occurrence, this month while still ahead")
{
  CHECK(fireAt("el 15 a las 5 de la tarde") == utc({.day = 15, .hour = 17}));
  CHECK(fireAt("el 15 a las cinco") == utc({.day = 15, .hour = 17}));
  CHECK(fireAt("el día 20 a las 8") == utc({.day = 20, .hour = 8}));
  CHECK(fireAt("el 30 a las 9") == utc({.day = 30, .hour = 9}));
  CHECK(fireAt("el 31 a las 10") == utc({.day = 31, .hour = 10}));
  CHECK(fireAt("el 7 a las 5") == utc(today(17)));
  CHECK(fireAt("el 7 a las 9") == utc({.month = 11, .day = 7, .hour = 9}));
  CHECK(fireAt("el 3 a las 9") == utc({.month = 11, .day = 3, .hour = 9}));
  CHECK(fireAt("el quince a las 3") == utc({.day = 15, .hour = 15}));
  CHECK(fireAt("on the 15th at 5 pm", "en") == utc({.day = 15, .hour = 17}));
  CHECK(fireAt("the 15th at 5", "en") == utc({.day = 15, .hour = 17}));
  CHECK(fireAt("on the fifteenth at 5 p.m.", "en") == utc({.day = 15, .hour = 17}));
  CHECK(fireAt("on the twenty first at 8 pm", "en") == utc({.day = 21, .hour = 20}));
  CHECK(fireAt("on the 7th at 5", "en") == utc(today(17)));
  CHECK(fireAt("on the 3rd at 9 am", "en") == utc({.month = 11, .day = 3, .hour = 9}));
  CHECK(fireAt("the 1st at 9 am", "en") == utc({.month = 11, .day = 1, .hour = 9}));
}

TEST_CASE("a day and a month name the date, up to twelve months ahead")
{
  CHECK(fireAt("el quince de octubre a las 3") == utc({.day = 15, .hour = 15}));
  CHECK(fireAt("el 15 de octubre a las 9 de la mañana") == utc({.day = 15, .hour = 9}));
  CHECK(fireAt("el 7 de octubre a las 4 de la tarde") == utc(today(16)));
  CHECK(fireAt("el 1 de noviembre a las 10") == utc({.month = 11, .day = 1, .hour = 10}));
  CHECK(fireAt("el primero de noviembre a las 10") == utc({.month = 11, .day = 1, .hour = 10}));
  CHECK(fireAt("el 15 de diciembre a las 4") == utc({.month = 12, .day = 15, .hour = 16}));
  CHECK(fireAt("el 3 de enero a las 9") == utc({.year = 2027, .month = 1, .day = 3, .hour = 9}));
  CHECK(fireAt("el 5 de octubre a las 9") == utc({.year = 2027, .month = 10, .day = 5, .hour = 9}));
  CHECK(fireAt("15 de octubre a las 5 pm") == utc({.day = 15, .hour = 17}));
  CHECK(fireAt("el 15 de setiembre a las 4") == utc({.year = 2027, .month = 9, .day = 15, .hour = 16}));
  CHECK(fireAt("October 15th at 3", "en") == utc({.day = 15, .hour = 15}));
  CHECK(fireAt("october 15 at 9 am", "en") == utc({.day = 15, .hour = 9}));
  CHECK(fireAt("oct 15th at 4 pm", "en") == utc({.day = 15, .hour = 16}));
  CHECK(fireAt("15th of October at 9 am", "en") == utc({.day = 15, .hour = 9}));
  CHECK(fireAt("march 3rd at 10 am", "en") == utc({.year = 2027, .month = 3, .day = 3, .hour = 10}));
  CHECK_FALSE(fireAt("el 31 de noviembre a las 9"));
  CHECK_FALSE(fireAt("el 30 de febrero a las 9"));
  CHECK_FALSE(fireAt("el 29 de febrero a las 9"));
}

TEST_CASE("a weekday that agrees with the date is resolved, one that contradicts it is not and names both days")
{
  CHECK(fireAt("el viernes 9 a las 5") == utc({.day = 9, .hour = 17}));
  CHECK(fireAt("el lunes 12 a las 10") == utc({.day = 12, .hour = 10}));
  CHECK(fireAt("el viernes 9 de octubre a las 5") == utc({.day = 9, .hour = 17}));
  CHECK(fireAt("on Friday the 9th at 5 pm", "en") == utc({.day = 9, .hour = 17}));
  CHECK_FALSE(fireAt("el lunes 9 a las 5"));
  CHECK(conflictOf("el lunes 9 a las 5") ==
        Conflict{.byWeekday = utc({.day = 12, .hour = 17}), .byDate = utc({.day = 9, .hour = 17})});
  CHECK(conflictOf("el viernes 10 a las 5") ==
        Conflict{.byWeekday = utc({.day = 9, .hour = 17}), .byDate = utc({.day = 10, .hour = 17})});
  CHECK(conflictOf("el domingo 9 a las 10 de la mañana") ==
        Conflict{.byWeekday = utc({.day = 11, .hour = 10}), .byDate = utc({.day = 9, .hour = 10})});
  CHECK(conflictOf("el lunes 5 a las 9") ==
        Conflict{.byWeekday = utc({.month = 11, .day = 2, .hour = 9}), .byDate = utc({.month = 11, .day = 5, .hour = 9})});
  CHECK(conflictOf("on Monday the 9th at 5 pm", "en") ==
        Conflict{.byWeekday = utc({.day = 12, .hour = 17}), .byDate = utc({.day = 9, .hour = 17})});
  CHECK_FALSE(conflictOf("el viernes 9 a las 5"));
  CHECK_FALSE(conflictOf("a las 5"));
  const std::string text = "llamar a Juan el lunes 9 a las 5";
  const DayConflict conflict = call_time::read({.text = text, .lang = "es", .now = now()}).conflict.value_or(DayConflict{});
  CHECK(call_time::withoutPhrase(text, {.fireAt = 0, .phraseBegin = conflict.phraseBegin, .phraseEnd = conflict.phraseEnd}) == "llamar a Juan");
}

TEST_CASE("a bare 7 to 11 whose morning has passed is tonight when no day is named, and the morning after when tonight has passed too")
{
  CHECK(fireAt("a las nueve") == utc(today(21)));
  CHECK(fireAt("a las ocho menos cuarto") == utc(today(19, 45)));
  CHECK(fireAt("at 9:15", "en") == utc(today(21, 15)));
  CHECK(fireAt("a las 9 de la mañana") == utc(tomorrow(9)));
  CHECK(fireAt("mañana a las nueve") == utc(tomorrow(9)));
  CHECK(fireAt("el viernes a las nueve") == utc({.day = 9, .hour = 9}));
  CHECK(fireAt("hoy a las nueve") == utc(today(21)));
  CHECK(fireAt("today at nine", "en") == utc(today(21)));
  CHECK(fireAt("hoy a las siete y media") == utc(today(19, 30)));
  const auto late = [](const std::string& text) {
    const auto found = call_time::resolve({.text = text, .lang = "es", .now = utc({.day = 7, .hour = 23, .minute = 40})});
    return found ? std::optional<int64_t>(found->fireAt) : std::nullopt;
  };
  CHECK(late("a las nueve") == utc(tomorrow(9)));
  CHECK(late("a las 7:30") == utc(tomorrow(7, 30)));
  CHECK(late("a las cinco") == utc(tomorrow(17)));
}

TEST_CASE("an explicit today whose every reading has passed proposes tomorrow at that hour and schedules nothing")
{
  const int64_t evening = utc({.day = 7, .hour = 21, .minute = 30});
  const auto proposed = [evening](const std::string& text, const std::string& lang = "es") {
    const CallReading reading = call_time::read({.text = text, .lang = lang, .now = evening});
    return std::make_pair(reading.time.has_value(), reading.passedToday.value_or(0));
  };
  CHECK(proposed("hoy a las ocho") == std::make_pair(false, utc(tomorrow(8))));
  CHECK(proposed("hoy a las nueve") == std::make_pair(false, utc(tomorrow(9))));
  CHECK(proposed("today at eight", "en") == std::make_pair(false, utc(tomorrow(8))));
  CHECK(proposed("hoy a las 9 de la mañana") == std::make_pair(false, utc(tomorrow(9))));
  CHECK(proposed("esta tarde a las 5") == std::make_pair(false, utc(tomorrow(17))));
  CHECK(proposed("hoy a las 5") == std::make_pair(false, utc(tomorrow(17))));
  CHECK(proposed("hoy a las 8 y media") == std::make_pair(false, utc(tomorrow(8, 30))));
  CHECK(proposed("hoy a las diez") == std::make_pair(true, int64_t{0}));
  CHECK(proposed("hoy a las 11 de la noche") == std::make_pair(true, int64_t{0}));
  CHECK(proposed("mañana a las ocho") == std::make_pair(true, int64_t{0}));
  const auto tonight = [evening](const std::string& text) {
    const auto found = call_time::resolve({.text = text, .lang = "es", .now = evening});
    return found ? std::optional<int64_t>(found->fireAt) : std::nullopt;
  };
  CHECK(tonight("a las ocho") == utc(tomorrow(8)));
  CHECK(tonight("a las diez") == utc(today(22)));
  CHECK_FALSE(tonight("hoy a las ocho"));
}

TEST_CASE("a weekday that disagrees with today, tomorrow or the day after is a question naming both days")
{
  CHECK(fireAt("mañana jueves a las 5") == utc(tomorrow(17)));
  CHECK(fireAt("hoy miércoles a las 5") == utc(today(17)));
  CHECK(fireAt("pasado mañana viernes a las 3") == utc({.day = 9, .hour = 15}));
  CHECK(fireAt("tomorrow Thursday at 5", "en") == utc(tomorrow(17)));
  CHECK_FALSE(fireAt("mañana lunes a las 5"));
  CHECK(conflictOf("mañana lunes a las 5") == Conflict{.byWeekday = utc({.day = 12, .hour = 17}), .byDate = utc(tomorrow(17))});
  CHECK(call_time::read({.text = "mañana lunes a las 5", .lang = "es", .now = now()}).conflict.value_or(DayConflict{}).relative == 1);
  CHECK(conflictOf("hoy lunes a las 5") == Conflict{.byWeekday = utc({.day = 12, .hour = 17}), .byDate = utc(today(17))});
  CHECK(call_time::read({.text = "hoy lunes a las 5", .lang = "es", .now = now()}).conflict.value_or(DayConflict{}).relative == 0);
  CHECK(conflictOf("pasado mañana lunes a las 5") == Conflict{.byWeekday = utc({.day = 12, .hour = 17}), .byDate = utc({.day = 9, .hour = 17})});
  CHECK(conflictOf("tomorrow Monday at 5 pm", "en") == Conflict{.byWeekday = utc({.day = 12, .hour = 17}), .byDate = utc(tomorrow(17))});
  CHECK(call_time::read({.text = "el lunes 9 a las 5", .lang = "es", .now = now()}).conflict.value_or(DayConflict{}).relative == -1);
}

TEST_CASE("an explicit year inside the twelve months is read, one outside is not resolved and says so")
{
  const auto far = [](const std::string& text, const std::string& lang = "es") {
    const CallReading reading = call_time::read({.text = text, .lang = lang, .now = now()});
    return reading.farAway && !reading.time && !reading.conflict;
  };
  CHECK(fireAt("el 15 de octubre de 2026 a las 5") == utc({.day = 15, .hour = 17}));
  CHECK(fireAt("el 3 de marzo de 2027 a las 10 de la mañana") == utc({.year = 2027, .month = 3, .day = 3, .hour = 10}));
  CHECK(fireAt("el 5 de octubre del 2027 a las 9") == utc({.year = 2027, .month = 10, .day = 5, .hour = 9}));
  CHECK(fireAt("October 15, 2026 at 5 pm", "en") == utc({.day = 15, .hour = 17}));
  CHECK(fireAt("march 3rd 2027 at 10 am", "en") == utc({.year = 2027, .month = 3, .day = 3, .hour = 10}));
  CHECK(fireAt("el viernes 9 de octubre de 2026 a las 5") == utc({.day = 9, .hour = 17}));
  CHECK(far("el 15 de octubre de 2027 a las 5"));
  CHECK(far("el 3 de marzo de 2029 a las 10 de la mañana"));
  CHECK(far("el 3 de marzo de 2025 a las 10 de la mañana"));
  CHECK(far("march 3rd 2029 at 9 am", "en"));
  CHECK(far("el 7 de octubre de 2026 a las 9 de la mañana"));
  CHECK_FALSE(far("en 900 horas"));
  CHECK_FALSE(far("el 31 de noviembre de 2026 a las 9"));
  CHECK_FALSE(far("el 15 de octubre"));
  CHECK(conflictOf("el lunes 9 de octubre de 2026 a las 5").has_value());
}

TEST_CASE("the local clock decides, so Lima time reads the same words at its own hours")
{
  setenv("TZ", "<-05>5", 1);
  tzset();
  const int64_t limaNow = utc({.day = 7, .hour = 15, .minute = 20});
  const auto at = [limaNow](const std::string& text) {
    const auto found = call_time::resolve({.text = text, .lang = "es", .now = limaNow});
    return found ? std::optional<int64_t>(found->fireAt) : std::nullopt;
  };
  CHECK(at("a las cinco") == utc({.day = 7, .hour = 22}));
  CHECK(at("a las nueve") == utc({.day = 8, .hour = 2}));
  CHECK(at("mañana a las 3 de la tarde") == utc({.day = 8, .hour = 20}));
  CHECK(at("el viernes a las cinco") == utc({.day = 9, .hour = 22}));
  CHECK(at("a las 11 de la noche") == utc({.day = 8, .hour = 4}));
  setenv("TZ", "UTC", 1);
  tzset();
}

namespace
{
constexpr int64_t kLimaOffsetS = int64_t{5} * 3600;

struct Lima
{
  Lima()
  {
    setenv("TZ", "<-05>5", 1);
    tzset();
  }

  Lima(const Lima&) = delete;
  Lima& operator=(const Lima&) = delete;

  ~Lima()
  {
    setenv("TZ", "UTC", 1);
    tzset();
  }

  [[nodiscard]] static int64_t at(const Moment& moment) { return utc(moment) + kLimaOffsetS; }

  [[nodiscard]] static int64_t reference() { return at({.day = 6, .hour = 12, .minute = 30}); }

  [[nodiscard]] static CallReading read(const std::string& text, const std::string& lang)
  {
    return call_time::read({.text = text, .lang = lang, .now = reference()});
  }
};

struct Phrase
{
  const char* id;
  const char* lang;
  const char* text;
  Moment expected;
};
}

TEST_CASE("the phrases of the extraction test split, read at Lima's Tuesday 2026-10-06 12:30 by the owner's rules")
{
  const Lima lima;
  const std::array<Phrase, 28> phrases{{
      {.id = "ct-01", .lang = "en", .text = "Monday at 6:45", .expected = {.day = 12, .hour = 18, .minute = 45}},
      {.id = "ct-02", .lang = "en", .text = "Sunday at 3", .expected = {.day = 11, .hour = 15}},
      {.id = "ct-03", .lang = "en", .text = "Wednesday at 12:30", .expected = {.day = 7, .hour = 12, .minute = 30}},
      {.id = "ct-04", .lang = "en", .text = "friday at five p. m.", .expected = {.day = 9, .hour = 17}},
      {.id = "ct-05", .lang = "en", .text = "friday at two hundred p. m.", .expected = {.day = 9, .hour = 14}},
      {.id = "ct-06", .lang = "en", .text = "march nineteen at one p. m.", .expected = {.year = 2027, .month = 3, .day = 19, .hour = 13}},
      {.id = "ct-07", .lang = "en", .text = "october twenty eighth at six thirty", .expected = {.day = 28, .hour = 18, .minute = 30}},
      {.id = "ct-08", .lang = "en", .text = "thursday at eleven fifteen", .expected = {.day = 8, .hour = 11, .minute = 15}},
      {.id = "ct-09", .lang = "en", .text = "tomorrow at two p. m.", .expected = {.day = 7, .hour = 14}},
      {.id = "ct-10", .lang = "es", .text = "doce de febrero a las seis de la tarde", .expected = {.year = 2027, .month = 2, .day = 12, .hour = 18}},
      {.id = "ct-11", .lang = "es", .text = "el 21 a las ocho", .expected = {.day = 21, .hour = 8}},
      {.id = "ct-12", .lang = "es", .text = "el 9 a las once", .expected = {.day = 9, .hour = 11}},
      {.id = "ct-13", .lang = "es", .text = "el doce de noviembre a las siete", .expected = {.month = 11, .day = 12, .hour = 7}},
      {.id = "ct-14", .lang = "es", .text = "el jueves a las 18:30", .expected = {.day = 8, .hour = 18, .minute = 30}},
      {.id = "ct-15", .lang = "es", .text = "el lunes a las cuatro", .expected = {.day = 12, .hour = 16}},
      {.id = "ct-16", .lang = "es", .text = "el lunes a las tres", .expected = {.day = 12, .hour = 15}},
      {.id = "ct-18", .lang = "es", .text = "el martes a las 8:15", .expected = {.day = 13, .hour = 8, .minute = 15}},
      {.id = "ct-19", .lang = "es", .text = "el martes a las cinco", .expected = {.day = 13, .hour = 17}},
      {.id = "ct-20", .lang = "es", .text = "el martes a las cinco y media", .expected = {.day = 13, .hour = 17, .minute = 30}},
      {.id = "ct-21", .lang = "es", .text = "el martes a las dos", .expected = {.day = 13, .hour = 14}},
      {.id = "ct-22", .lang = "es", .text = "el quince a las diez de la mañana", .expected = {.day = 15, .hour = 10}},
      {.id = "ct-23", .lang = "es", .text = "el veinte a las diez de la mañana", .expected = {.day = 20, .hour = 10}},
      {.id = "ct-24", .lang = "es", .text = "el viernes toca reunión del cole a las seis", .expected = {.day = 9, .hour = 18}},
      {.id = "ct-25", .lang = "es", .text = "jueves a la una", .expected = {.day = 8, .hour = 13}},
      {.id = "ct-26", .lang = "es", .text = "jueves a las dos p. m.", .expected = {.day = 8, .hour = 14}},
      {.id = "ct-27", .lang = "es", .text = "mañana a las cuatro y media", .expected = {.day = 7, .hour = 16, .minute = 30}},
      {.id = "ct-28", .lang = "es", .text = "mañana a las dos p. m.", .expected = {.day = 7, .hour = 14}},
      {.id = "ct-29", .lang = "es", .text = "pasado mañana a las cuatro", .expected = {.day = 8, .hour = 16}},
  }};
  for (const Phrase& phrase : phrases) {
    const auto found = Lima::read(phrase.text, phrase.lang).time;
    CHECK_MESSAGE(found.has_value(), phrase.id);
    CHECK_MESSAGE(found.value_or(CallTime{}).fireAt == Lima::at(phrase.expected), phrase.id);
  }
}

TEST_CASE("a weekday that disagrees with the day of the month is not resolved and names both days (ct-17)")
{
  const Lima lima;
  const CallReading reading = Lima::read("el martes 4 a las 8:05", "es");
  CHECK_FALSE(reading.time);
  const DayConflict conflict = reading.conflict.value_or(DayConflict{});
  CHECK(conflict.byWeekday == Lima::at({.month = 11, .day = 3, .hour = 8, .minute = 5}));
  CHECK(conflict.byDate == Lima::at({.month = 11, .day = 4, .hour = 8, .minute = 5}));
}

TEST_CASE("what the read-back says, said back to the resolver, is the same instant")
{
  const int64_t reference = now();
  const std::array<Moment, 6> days{{{.day = 8}, {.day = 9}, {.day = 12}, {.month = 11, .day = 5}, {.month = 12, .day = 25}, {.year = 2027, .month = 2, .day = 3}}};
  for (const std::string_view lang : {"es", "en"}) {
    for (const Moment& day : days) {
      for (int hour = 0; hour < 24; ++hour) {
        for (const int minute : {0, 15, 30, 45}) {
          const int64_t epoch = utc({.year = day.year, .month = day.month, .day = day.day, .hour = hour, .minute = minute});
          const std::string said = spoken_time::moment({.epoch = epoch, .now = reference, .lang = lang, .day = spoken_time::Day::Weekday});
          CHECK_MESSAGE(fireAt(said, std::string(lang)) == epoch, said);
        }
      }
    }
  }
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
  CHECK_FALSE(fireAt("el 15 de octubre"));
  CHECK_FALSE(fireAt("el viernes"));
  CHECK_FALSE(fireAt("en casa"));
  CHECK_FALSE(fireAt("a las 25"));
  CHECK_FALSE(fireAt("en 900 horas"));
  CHECK_FALSE(fireAt(""));
  CHECK_FALSE(fireAt("in the morning", "en"));
}

TEST_CASE("the time phrase is cut out of the topic, accents kept")
{
  const auto cut = [](const std::string& text) {
    const auto found = call_time::resolve({.text = text, .lang = "es", .now = now()});
    return found ? call_time::withoutPhrase(text, *found) : std::string("unresolved");
  };
  CHECK(cut("llamar al dentista mañana a las nueve") == "llamar al dentista");
  CHECK(cut("mañana a las nueve, llamar a mamá") == "llamar a mamá");
  CHECK(cut("llamar al dentista el viernes a las 5 de la tarde") == "llamar al dentista");
  CHECK(cut("llamar a mamá mañana a las 5 p. m.") == "llamar a mamá");
  CHECK(cut("recuérdame el 15 de octubre a las 5 llamar a Juan") == "recuérdame llamar a Juan");
  CHECK(cut("llamar a Pedro a las cinco y media de la tarde") == "llamar a Pedro");
  CHECK(cut("llamar a Pedro a mediodía") == "llamar a Pedro");
  const std::string english = "call mom at half past five";
  const auto found = call_time::resolve({.text = english, .lang = "en", .now = now()});
  REQUIRE(found);
  CHECK(call_time::withoutPhrase(english, found.value_or(CallTime{})) == "call mom");
  const std::string pm = "call mom at 5 pm";
  const auto evening = call_time::resolve({.text = pm, .lang = "en", .now = now()});
  REQUIRE(evening);
  CHECK(call_time::withoutPhrase(pm, evening.value_or(CallTime{})) == "call mom");
}
