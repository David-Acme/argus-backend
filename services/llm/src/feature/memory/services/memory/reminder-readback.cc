#include "reminder-readback.hxx"

#include <text/spoken-time.hxx>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace reminder_readback
{

namespace
{
std::string spokenAt(const Spoken& spoken, bool bare)
{
  return spoken_time::moment(
      {.epoch = spoken.fireAt, .now = spoken.now, .lang = spoken.lang, .day = spoken_time::Day::Weekday, .bare = bare});
}

std::string_view reasonOf(const Spoken& spoken)
{
  const bool english = spoken.lang == "en";
  switch (spoken.why) {
    case ReminderCallOutcome::TooFar:
      return english ? " I can only call you up to 12 months ahead." : " Solo puedo llamarte hasta dentro de 12 meses.";
    case ReminderCallOutcome::InThePast:
      return english ? " That time has already passed." : " Esa hora ya pasó.";
    case ReminderCallOutcome::TooMany:
      return english ? " You already have a lot of calls waiting." : " Ya tienes muchas llamadas pendientes.";
    case ReminderCallOutcome::Unavailable:
      return english ? " The call service is not answering right now." : " El servicio de llamadas no responde ahora.";
    case ReminderCallOutcome::Scheduled:
    case ReminderCallOutcome::Refused:
      break;
  }
  return {};
}
}

std::string moment(const Spoken& spoken)
{
  return spokenAt(spoken, false);
}

std::string sentence(const Spoken& spoken)
{
  const bool english = spoken.lang == "en";
  switch (spoken.call) {
    case Call::Scheduled:
      return (english ? " I will call you " : " Te llamaré ") + spokenAt(spoken, false) + ".";
    case Call::NotAttempted:
      return (english ? " It is in your reminders for " : " Quedó en tus recordatorios para ") + spokenAt(spoken, true) + ".";
    case Call::Failed:
      break;
  }
  if (english)
    return std::string(spoken.listed ? " I saved it in your reminders, but I could not schedule the call."
                                     : " I saved it, but I could not schedule the call.") +
           std::string(reasonOf(spoken)) + " It was for " + spokenAt(spoken, true) + ".";
  return std::string(spoken.listed ? " Lo guardé en tus recordatorios, pero no pude programar la llamada."
                                   : " Lo guardé, pero no pude programar la llamada.") +
         std::string(reasonOf(spoken)) + " Era para " + spokenAt(spoken, false) + ".";
}

namespace
{
constexpr std::array<std::string_view, 7> kEsDays{"lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo"};
constexpr std::array<std::string_view, 7> kEnDays{"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
constexpr std::array<std::string_view, 12> kEsMonths{"enero", "febrero", "marzo", "abril", "mayo", "junio",
                                                      "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
constexpr std::array<std::string_view, 12> kEnMonths{"January", "February", "March", "April", "May", "June",
                                                      "July", "August", "September", "October", "November", "December"};
constexpr std::array<int, 5> kWorkdays{1, 2, 3, 4, 5};
constexpr std::array<int, 2> kWeekend{0, 6};
constexpr int kWeekDays = 7;

std::tm localOf(int64_t epoch)
{
  const auto seconds = static_cast<std::time_t>(epoch);
  std::tm local{};
  localtime_r(&seconds, &local);
  return local;
}

std::string_view dayName(int weekday, bool english)
{
  const auto index = static_cast<std::size_t>((weekday + kWeekDays - 1) % kWeekDays);
  return english ? kEnDays.at(index) : kEsDays.at(index);
}

std::string pluralDay(int weekday, bool english)
{
  std::string name(dayName(weekday, english));
  if (english || weekday == 0 || weekday == kWeekDays - 1)
    name += 's';
  return name;
}

std::string joined(const std::vector<std::string>& names, bool english)
{
  std::string out;
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (index > 0)
      out += index + 1 == names.size() ? (english ? " and " : " y ") : ", ";
    out += names[index];
  }
  return out;
}

template <std::size_t N>
bool sameDays(const std::vector<int>& days, const std::array<int, N>& expected)
{
  return std::ranges::equal(days, expected);
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

std::string weeklyEs(const Repeat& repeat, const std::vector<int>& days)
{
  std::vector<std::string> names;
  names.reserve(days.size());
  for (const int day : days)
    names.push_back(pluralDay(day, false));
  std::string which;
  if (sameDays(days, kWorkdays))
    which = "de lunes a viernes";
  else if (sameDays(days, kWeekend))
    which = "los fines de semana";
  else if (days.size() == 1 && repeat.interval == 1)
    return "cada " + std::string(dayName(days.front(), false));
  else
    which = "los " + joined(names, false);
  if (repeat.interval == 1)
    return which;
  return "cada " + std::to_string(repeat.interval) + " semanas, " + which;
}

std::string weeklyEn(const Repeat& repeat, const std::vector<int>& days)
{
  std::vector<std::string> names;
  names.reserve(days.size());
  for (const int day : days)
    names.emplace_back(dayName(day, true));
  if (repeat.interval == 1) {
    if (sameDays(days, kWorkdays))
      return "every weekday";
    if (sameDays(days, kWeekend))
      return "every weekend";
    return "every " + joined(names, true);
  }
  return "every " + std::to_string(repeat.interval) + " weeks on " + joined(names, true);
}

std::string cadence(const RepeatSpoken& spoken)
{
  const bool english = spoken.lang == "en";
  const Repeat& repeat = spoken.repeat;
  const std::tm first = localOf(spoken.firstAt);
  const std::string every = std::to_string(repeat.interval);
  switch (repeat.frequency) {
    case RepeatFrequency::Daily:
      if (repeat.interval == 1)
        return english ? "every day" : "todos los días";
      return english ? "every " + every + " days" : "cada " + every + " días";
    case RepeatFrequency::Weekly: {
      const std::vector<int> days = repeat.weekdays.empty() ? std::vector<int>{first.tm_wday} : repeat.weekdays;
      return english ? weeklyEn(repeat, days) : weeklyEs(repeat, days);
    }
    case RepeatFrequency::Monthly: {
      const int day = repeat.monthDay > 0 ? repeat.monthDay : first.tm_mday;
      if (english)
        return repeat.interval == 1 ? "on the " + ordinal(day) + " of every month"
                                    : "on the " + ordinal(day) + " every " + every + " months";
      return repeat.interval == 1 ? "el día " + std::to_string(day) + " de cada mes"
                                  : "el día " + std::to_string(day) + ", cada " + every + " meses";
    }
    case RepeatFrequency::Yearly: {
      const auto month = static_cast<std::size_t>(first.tm_mon);
      if (english) {
        const std::string date = std::string(kEnMonths.at(month)) + " " + ordinal(first.tm_mday);
        return repeat.interval == 1 ? "every year on " + date : "every " + every + " years on " + date;
      }
      const std::string date = "el " + std::to_string(first.tm_mday) + " de " + std::string(kEsMonths.at(month));
      return repeat.interval == 1 ? "cada año, " + date : "cada " + every + " años, " + date;
    }
  }
  return {};
}
}

std::string repeatMoment(const RepeatSpoken& spoken)
{
  return cadence(spoken) + " " + spoken_time::clock({.epoch = spoken.firstAt, .now = spoken.now, .lang = spoken.lang, .day = spoken_time::Day::Relative, .bare = false});
}

std::string repeatSentence(const RepeatSpoken& spoken)
{
  const bool english = spoken.lang == "en";
  if (!spoken.listed)
    return english ? " I saved it, but I could not add it to your reminders, so it will not repeat."
                   : " Lo guardé, pero no pude dejarlo en tus recordatorios, así que no se repetirá.";
  const std::string firstDay =
      spoken_time::day({.epoch = spoken.firstAt, .now = spoken.now, .lang = spoken.lang, .day = spoken_time::Day::Relative, .bare = false});
  if (english)
    return " I will remind you " + repeatMoment(spoken) + ", starting " + firstDay + ".";
  return " Te lo recordaré " + repeatMoment(spoken) + "; la primera vez será " + firstDay + ".";
}

std::string_view untimedRepeat(std::string_view lang)
{
  return lang == "en" ? " You did not say at what time, so it is not in your reminders yet."
                      : " No me dijiste a qué hora, así que todavía no quedó en tus recordatorios.";
}

std::string_view unreadableRepeat(std::string_view lang)
{
  return lang == "en" ? "I did not understand how often to repeat it, so I did not save the reminder."
                      : "No entendí cada cuánto repetirlo, así que no guardé el recordatorio.";
}

}
