#include "spoken-time.hxx"

#include <array>
#include <ctime>

namespace spoken_time
{

namespace
{
constexpr std::array<std::string_view, 7> kEsDays{"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
constexpr std::array<std::string_view, 7> kEnDays{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr std::array<std::string_view, 12> kEsMonths{"enero", "febrero", "marzo", "abril", "mayo", "junio",
                                                     "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
constexpr std::array<std::string_view, 12> kEnMonths{"January", "February", "March",     "April",   "May",      "June",
                                                     "July",    "August",   "September", "October", "November", "December"};
constexpr int kNoon = 12;
constexpr int kFirstMorningHour = 6;
constexpr int kFirstEveningHour = 19;

std::tm localOf(int64_t epoch)
{
  const auto seconds = static_cast<std::time_t>(epoch);
  std::tm local{};
  localtime_r(&seconds, &local);
  return local;
}

bool sameDay(const std::tm& first, const std::tm& second)
{
  return first.tm_year == second.tm_year && first.tm_yday == second.tm_yday;
}

std::string ordinal(int day)
{
  const int tens = day % 100;
  if (tens >= 11 && tens <= 13)
    return std::to_string(day) + "th";
  switch (day % 10) {
    case 1:
      return std::to_string(day) + "st";
    case 2:
      return std::to_string(day) + "nd";
    case 3:
      return std::to_string(day) + "rd";
    default:
      return std::to_string(day) + "th";
  }
}

bool inOtherMonth(const std::tm& then, const std::tm& today)
{
  return then.tm_year != today.tm_year || then.tm_mon != today.tm_mon;
}

std::string_view esPeriod(int hour)
{
  if (hour < kFirstMorningHour)
    return "de la madrugada";
  if (hour < kNoon)
    return "de la mañana";
  return hour < kFirstEveningHour ? "de la tarde" : "de la noche";
}
}

std::string weekdayDate(const When& when)
{
  const std::tm then = localOf(when.epoch);
  const std::tm today = localOf(when.now);
  const auto weekday = static_cast<std::size_t>(then.tm_wday);
  const auto month = static_cast<std::size_t>(then.tm_mon);
  const bool other = inOtherMonth(then, today);
  if (when.lang == "en")
    return other ? std::string(kEnDays.at(weekday)) + ", " + std::string(kEnMonths.at(month)) + " " + ordinal(then.tm_mday)
                 : std::string(kEnDays.at(weekday)) + " the " + ordinal(then.tm_mday);
  return std::string(kEsDays.at(weekday)) + " " + std::to_string(then.tm_mday) +
         (other ? " de " + std::string(kEsMonths.at(month)) : std::string());
}

std::string day(const When& when)
{
  const bool english = when.lang == "en";
  if (when.day == Day::Relative) {
    const std::tm then = localOf(when.epoch);
    const std::tm today = localOf(when.now);
    if (sameDay(then, today))
      return english ? "today" : "hoy";
    std::tm following = today;
    following.tm_mday += 1;
    following.tm_isdst = -1;
    const std::time_t next = std::mktime(&following);
    if (sameDay(then, localOf(static_cast<int64_t>(next))))
      return english ? "tomorrow" : "mañana";
  }
  const std::string date = weekdayDate(when);
  if (english)
    return when.bare ? date : "on " + date;
  return "el " + date;
}

std::string clock(const When& when)
{
  const std::tm then = localOf(when.epoch);
  const bool english = when.lang == "en";
  if (then.tm_hour == kNoon && then.tm_min == 0)
    return english ? "at noon" : "a mediodía";
  const int hour12 = then.tm_hour % kNoon == 0 ? kNoon : then.tm_hour % kNoon;
  std::string text = std::to_string(hour12);
  if (then.tm_min != 0)
    text += (then.tm_min < 10 ? ":0" : ":") + std::to_string(then.tm_min);
  if (english)
    return "at " + text + (then.tm_hour < kNoon ? " AM" : " PM");
  return std::string(hour12 == 1 ? "a la " : "a las ") + text + " " + std::string(esPeriod(then.tm_hour));
}

std::string moment(const When& when)
{
  return day(when) + " " + clock(when);
}

}
