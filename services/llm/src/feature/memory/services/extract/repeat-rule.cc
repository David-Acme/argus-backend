#include "repeat-rule.hxx"

#include "call-time-words.hxx"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>

namespace repeat_rule
{

namespace
{
using call_time::anyWordAt;
using call_time::Tokens;
using call_time::wordAt;

constexpr int kWeekDays = 7;
constexpr int kMinutesInHour = 60;
constexpr int kNoon = 12;
constexpr int kMonthsInYear = 12;
constexpr int kMonthsAhead = 24;
constexpr int kLastMonthDay = 31;
constexpr int kFortnightDays = 15;
constexpr int kEveryOther = 2;
constexpr std::array<int, 5> kWorkdays{1, 2, 3, 4, 5};
constexpr std::array<int, 2> kWeekend{0, 6};
constexpr std::array<std::string_view, 7> kByDay{"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
constexpr std::array<std::string_view, 7> kEnglishPlurals{"sundays",   "mondays", "tuesdays", "wednesdays",
                                                          "thursdays", "fridays", "saturdays"};

enum class Unit : std::uint8_t
{
  None,
  Day,
  Week,
  Month,
  Year,
  Weekday,
  Part,
  Weekend,
  Workdays,
  Fortnight,
  Unsupported
};

struct UnitHit
{
  Unit unit{Unit::None};
  DayPart part{DayPart::None};
  std::size_t length{1};
};

int weekdayAt(const Tokens& tokens, std::size_t at)
{
  if (at >= tokens.size())
    return -1;
  const std::string& text = tokens[at].text;
  if (const int day = call_time::weekdayOf(text); day >= 0)
    return day;
  if (text.size() > 1 && text.back() == 's')
    return call_time::weekdayOf(text.substr(0, text.size() - 1));
  return -1;
}

bool englishPlural(const Tokens& tokens, std::size_t at)
{
  return at < tokens.size() && std::ranges::find(kEnglishPlurals, std::string_view(tokens[at].text)) != kEnglishPlurals.end();
}

std::size_t ledBy(const Tokens& tokens, std::size_t at)
{
  return at > 0 && wordAt(tokens, at - 1, "on") ? at - 1 : at;
}

bool articled(const Tokens& tokens, std::size_t at)
{
  return at > 0 && anyWordAt(tokens, at - 1, {"el", "mi", "tu", "su", "un", "del", "al", "este", "ese", "the", "a", "my",
                                              "our", "your", "his", "her", "their", "this", "that"});
}

UnitHit unitAt(const Tokens& tokens, std::size_t at)
{
  if (at >= tokens.size())
    return {};
  if (anyWordAt(tokens, at, {"dia", "dias", "day", "days"})) {
    if (anyWordAt(tokens, at + 1, {"habiles", "laborables", "utiles"}))
      return {.unit = Unit::Workdays, .part = DayPart::None, .length = 2};
    if (wordAt(tokens, at + 1, "de") && wordAt(tokens, at + 2, "semana"))
      return {.unit = Unit::Workdays, .part = DayPart::None, .length = 3};
    return {.unit = Unit::Day, .part = DayPart::None, .length = 1};
  }
  if (anyWordAt(tokens, at, {"semana", "semanas", "week", "weeks"}))
    return {.unit = Unit::Week, .part = DayPart::None, .length = 1};
  if (anyWordAt(tokens, at, {"mes", "meses", "month", "months"}))
    return {.unit = Unit::Month, .part = DayPart::None, .length = 1};
  if (anyWordAt(tokens, at, {"ano", "anos", "year", "years"}))
    return {.unit = Unit::Year, .part = DayPart::None, .length = 1};
  if (anyWordAt(tokens, at, {"quincena", "quincenas"}))
    return {.unit = Unit::Fortnight, .part = DayPart::None, .length = 1};
  if (anyWordAt(tokens, at, {"fin", "fines"}) && wordAt(tokens, at + 1, "de") && wordAt(tokens, at + 2, "semana"))
    return {.unit = Unit::Weekend, .part = DayPart::None, .length = 3};
  if (anyWordAt(tokens, at, {"weekend", "weekends"}))
    return {.unit = Unit::Weekend, .part = DayPart::None, .length = 1};
  if (anyWordAt(tokens, at, {"weekday", "weekdays", "workday", "workdays"}))
    return {.unit = Unit::Workdays, .part = DayPart::None, .length = 1};
  if (wordAt(tokens, at, "business") && anyWordAt(tokens, at + 1, {"day", "days"}))
    return {.unit = Unit::Workdays, .part = DayPart::None, .length = 2};
  if (weekdayAt(tokens, at) >= 0)
    return {.unit = Unit::Weekday, .part = DayPart::None, .length = 1};
  if (anyWordAt(tokens, at, {"manana", "mananas", "morning", "mornings"}))
    return {.unit = Unit::Part, .part = DayPart::Morning, .length = 1};
  if (anyWordAt(tokens, at, {"tarde", "tardes", "afternoon", "afternoons", "evening", "evenings"}))
    return {.unit = Unit::Part, .part = DayPart::Afternoon, .length = 1};
  if (anyWordAt(tokens, at, {"noche", "noches", "night", "nights"}))
    return {.unit = Unit::Part, .part = DayPart::Night, .length = 1};
  if (anyWordAt(tokens, at, {"hora", "horas", "minuto", "minutos", "segundo", "segundos", "rato", "ratito", "momento",
                             "instante", "tanto", "cierto", "hour", "hours", "minute", "minutes", "second", "seconds",
                             "while", "now", "so", "often"}))
    return {.unit = Unit::Unsupported, .part = DayPart::None, .length = 1};
  return {};
}

std::optional<int> countAt(const Tokens& tokens, std::size_t at)
{
  if (at >= tokens.size())
    return std::nullopt;
  if (wordAt(tokens, at, "other"))
    return kEveryOther;
  if (wordAt(tokens, at, "second") && unitAt(tokens, at + 1).unit != Unit::None)
    return kEveryOther;
  if (anyWordAt(tokens, at, {"a", "an", "un", "una", "uno", "one"}))
    return std::nullopt;
  const int value = call_time::smallNumber(tokens[at].text);
  if (value < 1)
    return std::nullopt;
  return value;
}

struct Found
{
  std::optional<int> daily;
  std::optional<int> weekly;
  std::optional<int> monthly;
  std::optional<int> yearly;
  std::vector<int> weekdays;
  DayPart part{DayPart::None};
  int monthDay{0};
  bool unsupported{false};
  bool exception{false};
  bool nthWeekday{false};
  bool any{false};
  std::size_t first{std::numeric_limits<std::size_t>::max()};
  std::size_t last{0};
};

void setOnce(std::optional<int>& slot, int value)
{
  if (!slot)
    slot = value;
}

struct Span
{
  std::size_t first{0};
  std::size_t last{0};
};

struct Applied
{
  const UnitHit& hit;
  std::size_t at{0};
  int count{1};
};

struct Frequent
{
  const UnitHit& hit;
  Span span;
  bool once{false};
};

class Reader
{
public:
  explicit Reader(const Tokens& tokens) : tokens_(tokens) {}

  [[nodiscard]] Found run()
  {
    std::size_t index = 0;
    while (index < tokens_.size()) {
      if (anyWordAt(tokens_, index, {"excepto", "salvo", "except"}))
        found_.exception = true;
      if (anyWordAt(tokens_, index, {"primer", "primero", "segundo", "tercer", "tercero", "cuarto", "ultimo", "first", "second",
                                     "third", "fourth", "last"}) &&
          weekdayAt(tokens_, index + 1) >= 0)
        found_.nthWeekday = true;
      std::size_t used = every(index);
      if (used == 0)
        used = all(index);
      if (used == 0)
        used = range(index);
      if (used == 0)
        used = plural(index);
      if (used == 0)
        used = adverb(index);
      if (used == 0)
        used = times(index);
      if (used == 0)
        used = vague(index);
      index += std::max<std::size_t>(used, 1);
    }
    if (found_.monthly && found_.monthDay == 0)
      monthDay();
    return found_;
  }

private:
  void mark(const Span& span)
  {
    found_.first = std::min(found_.first, span.first);
    found_.last = std::max(found_.last, span.last);
    found_.any = true;
  }

  void addDay(int day)
  {
    if (day >= 0 && std::ranges::find(found_.weekdays, day) == found_.weekdays.end())
      found_.weekdays.push_back(day);
  }

  template <std::size_t N>
  void addDays(const std::array<int, N>& days)
  {
    for (const int day : days)
      addDay(day);
  }

  std::optional<std::size_t> rangeAt(std::size_t at)
  {
    if (!anyWordAt(tokens_, at + 1, {"a", "al", "to", "through", "thru", "until", "till", "hasta"}))
      return std::nullopt;
    const int from = weekdayAt(tokens_, at);
    const int to = weekdayAt(tokens_, at + 2);
    if (from < 0 || to < 0)
      return std::nullopt;
    for (int day = from;; day = (day + 1) % kWeekDays) {
      addDay(day);
      if (day == to)
        break;
    }
    return at + 2;
  }

  std::size_t list(std::size_t at)
  {
    if (const auto ranged = rangeAt(at))
      return *ranged;
    addDay(weekdayAt(tokens_, at));
    std::size_t last = at;
    std::size_t next = at + 1;
    while (next < tokens_.size()) {
      std::size_t probe = next;
      while (probe < tokens_.size() && anyWordAt(tokens_, probe, {"y", "e", "and", "los", "el", "on", "the"}))
        ++probe;
      if (weekdayAt(tokens_, probe) < 0)
        break;
      if (const auto ranged = rangeAt(probe)) {
        last = *ranged;
        next = last + 1;
        continue;
      }
      addDay(weekdayAt(tokens_, probe));
      last = probe;
      next = probe + 1;
    }
    return last;
  }

  std::size_t apply(const Applied& applied)
  {
    const UnitHit& hit = applied.hit;
    const std::size_t at = applied.at;
    const int count = applied.count;
    const std::size_t end = at + hit.length - 1;
    switch (hit.unit) {
      case Unit::Day:
        setOnce(found_.daily, count);
        return end;
      case Unit::Week:
        setOnce(found_.weekly, count);
        return end;
      case Unit::Month:
        setOnce(found_.monthly, count);
        return end;
      case Unit::Year:
        setOnce(found_.yearly, count);
        return end;
      case Unit::Fortnight:
        setOnce(found_.daily, kFortnightDays * count);
        return end;
      case Unit::Part:
        setOnce(found_.daily, count);
        if (found_.part == DayPart::None)
          found_.part = hit.part;
        return end;
      case Unit::Weekend:
        addDays(kWeekend);
        setOnce(found_.weekly, count);
        return end;
      case Unit::Workdays:
        addDays(kWorkdays);
        setOnce(found_.weekly, count);
        return end;
      case Unit::Weekday:
        setOnce(found_.weekly, count);
        return list(at);
      case Unit::Unsupported:
        found_.unsupported = true;
        return end;
      case Unit::None:
        break;
    }
    return at;
  }

  std::size_t every(std::size_t at)
  {
    if (!anyWordAt(tokens_, at, {"cada", "every", "each"}))
      return 0;
    std::size_t next = at + 1;
    const std::optional<int> count = countAt(tokens_, next);
    if (count && *count <= kLastMonthDay && wordAt(tokens_, next + 1, "de")) {
      std::size_t month = next + 2;
      if (wordAt(tokens_, month, "cada"))
        ++month;
      if (wordAt(tokens_, month, "mes")) {
        setOnce(found_.monthly, 1);
        found_.monthDay = *count;
        mark({.first = at, .last = month});
        return month - at + 1;
      }
    }
    if (count)
      ++next;
    const UnitHit hit = unitAt(tokens_, next);
    if (hit.unit == Unit::None) {
      if (!count)
        return 0;
      found_.unsupported = true;
      mark({.first = at, .last = next - 1});
      return next - at;
    }
    const std::size_t last = apply({.hit = hit, .at = next, .count = count.value_or(1)});
    mark({.first = at, .last = last});
    return last - at + 1;
  }

  std::size_t all(std::size_t at)
  {
    if (!anyWordAt(tokens_, at, {"todos", "todas"}) || !anyWordAt(tokens_, at + 1, {"los", "las"}))
      return 0;
    const UnitHit hit = unitAt(tokens_, at + 2);
    if (hit.unit == Unit::None || hit.unit == Unit::Unsupported)
      return 0;
    const std::size_t last = apply({.hit = hit, .at = at + 2, .count = 1});
    mark({.first = at, .last = last});
    return last - at + 1;
  }

  std::size_t range(std::size_t at)
  {
    if (anyWordAt(tokens_, at, {"de", "del", "from"}) && weekdayAt(tokens_, at + 1) >= 0) {
      if (const auto ranged = rangeAt(at + 1)) {
        setOnce(found_.weekly, 1);
        mark({.first = at, .last = *ranged});
        return *ranged - at + 1;
      }
    }
    if (weekdayAt(tokens_, at) >= 0 && anyWordAt(tokens_, at + 1, {"through", "thru"})) {
      if (const auto ranged = rangeAt(at)) {
        setOnce(found_.weekly, 1);
        mark({.first = at, .last = *ranged});
        return *ranged - at + 1;
      }
    }
    return 0;
  }

  std::size_t plural(std::size_t at)
  {
    if (wordAt(tokens_, at, "los")) {
      const UnitHit hit = unitAt(tokens_, at + 1);
      if (hit.unit == Unit::Weekday || hit.unit == Unit::Weekend || hit.unit == Unit::Workdays) {
        const std::size_t last = apply({.hit = hit, .at = at + 1, .count = 1});
        mark({.first = at, .last = last});
        return last - at + 1;
      }
      return 0;
    }
    if (wordAt(tokens_, at, "entre") && wordAt(tokens_, at + 1, "semana")) {
      addDays(kWorkdays);
      setOnce(found_.weekly, 1);
      mark({.first = at, .last = at + 1});
      return 2;
    }
    if (englishPlural(tokens_, at)) {
      setOnce(found_.weekly, 1);
      const std::size_t last = list(at);
      mark({.first = ledBy(tokens_, at), .last = last});
      return last - at + 1;
    }
    if (anyWordAt(tokens_, at, {"dias", "fines", "weekends", "weekdays", "workdays", "business"})) {
      const UnitHit hit = unitAt(tokens_, at);
      if (hit.unit == Unit::Weekend || hit.unit == Unit::Workdays) {
        const std::size_t last = apply({.hit = hit, .at = at, .count = 1});
        mark({.first = ledBy(tokens_, at), .last = last});
        return last - at + 1;
      }
    }
    return 0;
  }

  std::size_t adverb(std::size_t at)
  {
    if (wordAt(tokens_, at, "diario")) {
      if (!(at > 0 && wordAt(tokens_, at - 1, "a")) && articled(tokens_, at))
        return 0;
      setOnce(found_.daily, 1);
      mark({.first = at > 0 && wordAt(tokens_, at - 1, "a") ? at - 1 : at, .last = at});
      return 1;
    }
    if (anyWordAt(tokens_, at, {"diariamente", "everyday"})) {
      setOnce(found_.daily, 1);
      mark({.first = at, .last = at});
      return 1;
    }
    if (anyWordAt(tokens_, at, {"interdiario", "interdiaria"})) {
      setOnce(found_.daily, kEveryOther);
      mark({.first = at, .last = at});
      return 1;
    }
    if (wordAt(tokens_, at, "dia")) {
      std::size_t next = at + 1;
      if (wordAt(tokens_, next, "de"))
        ++next;
      if (wordAt(tokens_, next, "por") && wordAt(tokens_, next + 1, "medio")) {
        setOnce(found_.daily, kEveryOther);
        mark({.first = at, .last = next + 1});
        return next + 2 - at;
      }
      if (wordAt(tokens_, at + 1, "si") && wordAt(tokens_, at + 2, "y")) {
        std::size_t last = at + 2;
        for (std::size_t probe = at + 3; probe < tokens_.size() && probe <= at + 5; ++probe)
          if (wordAt(tokens_, probe, "no")) {
            last = probe;
            break;
          }
        setOnce(found_.daily, kEveryOther);
        mark({.first = at > 0 && wordAt(tokens_, at - 1, "un") ? at - 1 : at, .last = last});
        return last - at + 1;
      }
      return 0;
    }
    if (wordAt(tokens_, at, "daily") && !articled(tokens_, at)) {
      setOnce(found_.daily, 1);
      mark({.first = at, .last = at});
      return 1;
    }
    if (wordAt(tokens_, at, "semanalmente") || (wordAt(tokens_, at, "weekly") && !articled(tokens_, at))) {
      setOnce(found_.weekly, 1);
      mark({.first = at, .last = at});
      return 1;
    }
    if (wordAt(tokens_, at, "mensualmente") || (wordAt(tokens_, at, "monthly") && !articled(tokens_, at))) {
      setOnce(found_.monthly, 1);
      mark({.first = at, .last = at});
      return 1;
    }
    if (anyWordAt(tokens_, at, {"anualmente", "annually"}) || (wordAt(tokens_, at, "yearly") && !articled(tokens_, at))) {
      setOnce(found_.yearly, 1);
      mark({.first = at, .last = at});
      return 1;
    }
    return 0;
  }

  bool frequent(const Frequent& input)
  {
    const Unit unit = input.hit.unit;
    if (unit != Unit::Day && unit != Unit::Week && unit != Unit::Month && unit != Unit::Year)
      return false;
    if (input.once)
      apply({.hit = input.hit, .at = input.span.last, .count = 1});
    else
      found_.unsupported = true;
    mark(input.span);
    return true;
  }

  std::size_t times(std::size_t at)
  {
    if (anyWordAt(tokens_, at, {"vez", "veces"})) {
      std::size_t next = at + 1;
      if (anyWordAt(tokens_, next, {"al", "por"}))
        ++next;
      else if (wordAt(tokens_, next, "a") && wordAt(tokens_, next + 1, "la"))
        next += 2;
      else
        return 0;
      const bool once = wordAt(tokens_, at, "vez") && at > 0 && wordAt(tokens_, at - 1, "una");
      const UnitHit hit = unitAt(tokens_, next);
      const Span span{.first = at > 0 ? at - 1 : at, .last = next};
      return frequent({.hit = hit, .span = span, .once = once}) ? next - at + 1 : 0;
    }
    if (anyWordAt(tokens_, at, {"once", "twice", "thrice", "times"}) && anyWordAt(tokens_, at + 1, {"a", "per"})) {
      const bool once = wordAt(tokens_, at, "once");
      const UnitHit hit = unitAt(tokens_, at + 2);
      const Span span{.first = wordAt(tokens_, at, "times") && at > 0 ? at - 1 : at, .last = at + 2};
      return frequent({.hit = hit, .span = span, .once = once}) ? 3 : 0;
    }
    return 0;
  }

  std::size_t vague(std::size_t at)
  {
    if (wordAt(tokens_, at, "de") && wordAt(tokens_, at + 1, "vez") && wordAt(tokens_, at + 2, "en") &&
        wordAt(tokens_, at + 3, "cuando")) {
      found_.unsupported = true;
      mark({.first = at, .last = at + 3});
      return 4;
    }
    if (wordAt(tokens_, at, "from") && wordAt(tokens_, at + 1, "time") && wordAt(tokens_, at + 2, "to") &&
        wordAt(tokens_, at + 3, "time")) {
      found_.unsupported = true;
      mark({.first = at, .last = at + 3});
      return 4;
    }
    return 0;
  }

  void monthDay()
  {
    for (std::size_t at = 0; at < tokens_.size(); ++at) {
      if (!anyWordAt(tokens_, at, {"el", "los", "the", "dia"}))
        continue;
      std::size_t next = at + 1;
      if (anyWordAt(tokens_, at, {"el", "los"}) && wordAt(tokens_, next, "dia"))
        ++next;
      const auto number = call_time::dayNumberAt(tokens_, next);
      if (!number || (wordAt(tokens_, at, "the") && !number->ordinal))
        continue;
      const std::size_t after = next + number->length;
      std::size_t month = after;
      if (anyWordAt(tokens_, month, {"de", "del", "of"}))
        ++month;
      if (month < tokens_.size() && call_time::monthOf(tokens_[month].text) > 0)
        continue;
      found_.monthDay = number->value;
      mark({.first = ledBy(tokens_, at), .last = after - 1});
      return;
    }
  }

  const Tokens& tokens_;
  Found found_;
};

std::optional<Repeat> decided(const Found& found, bool monthNamed)
{
  if (found.unsupported || found.exception || (found.nthWeekday && (found.monthly || found.yearly)))
    return std::nullopt;
  if (!found.weekdays.empty()) {
    if (found.monthly || found.yearly || found.daily.value_or(1) != 1)
      return std::nullopt;
    Repeat repeat{.frequency = RepeatFrequency::Weekly, .interval = found.weekly.value_or(1), .weekdays = found.weekdays, .monthDay = 0};
    std::ranges::sort(repeat.weekdays);
    if (repeat.weekdays.size() == static_cast<std::size_t>(kWeekDays) && repeat.interval == 1)
      return Repeat{.frequency = RepeatFrequency::Daily, .interval = 1, .weekdays = {}, .monthDay = 0};
    return repeat;
  }
  if (found.monthly) {
    if (found.daily || found.weekly || found.yearly)
      return std::nullopt;
    return Repeat{.frequency = RepeatFrequency::Monthly, .interval = *found.monthly, .weekdays = {}, .monthDay = found.monthDay};
  }
  if (found.yearly) {
    if (found.daily || found.weekly || !monthNamed)
      return std::nullopt;
    return Repeat{.frequency = RepeatFrequency::Yearly, .interval = *found.yearly, .weekdays = {}, .monthDay = 0};
  }
  if (found.daily) {
    if (found.weekly)
      return std::nullopt;
    return Repeat{.frequency = RepeatFrequency::Daily, .interval = *found.daily, .weekdays = {}, .monthDay = 0};
  }
  if (found.weekly)
    return Repeat{.frequency = RepeatFrequency::Weekly, .interval = *found.weekly, .weekdays = {}, .monthDay = 0};
  return std::nullopt;
}

std::string_view frequencyName(RepeatFrequency frequency)
{
  switch (frequency) {
    case RepeatFrequency::Daily:
      return "DAILY";
    case RepeatFrequency::Weekly:
      return "WEEKLY";
    case RepeatFrequency::Monthly:
      return "MONTHLY";
    case RepeatFrequency::Yearly:
      return "YEARLY";
  }
  return "DAILY";
}

std::optional<RepeatFrequency> frequencyNamed(std::string_view name)
{
  constexpr std::array<RepeatFrequency, 4> kAll{RepeatFrequency::Daily, RepeatFrequency::Weekly, RepeatFrequency::Monthly,
                                                RepeatFrequency::Yearly};
  const auto found = std::ranges::find_if(kAll, [name](RepeatFrequency frequency) { return frequencyName(frequency) == name; });
  if (found == kAll.end())
    return std::nullopt;
  return *found;
}

struct Bounded
{
  std::string_view text;
  int high{1};
};

std::optional<int> numberIn(const Bounded& bounded)
{
  const std::string_view text = bounded.text;
  int value = 0;
  const char* last = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), last, value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != last || value < 1 || value > bounded.high)
    return std::nullopt;
  return value;
}

std::optional<std::vector<int>> weekdaysIn(std::string_view text)
{
  std::vector<int> days;
  while (true) {
    const std::size_t comma = text.find(',');
    const std::string_view part = text.substr(0, comma);
    const auto found = std::ranges::find(kByDay, part);
    if (found == kByDay.end())
      return std::nullopt;
    const auto day = static_cast<int>(std::distance(kByDay.begin(), found));
    if (std::ranges::find(days, day) != days.end())
      return std::nullopt;
    days.push_back(day);
    if (comma == std::string_view::npos)
      break;
    text = text.substr(comma + 1);
  }
  std::ranges::sort(days);
  return days;
}

struct Civil
{
  int year{0};
  int month{0};
  int day{0};
};

Civil civilOf(int64_t epoch)
{
  const auto seconds = static_cast<std::time_t>(epoch);
  std::tm local{};
  localtime_r(&seconds, &local);
  return {.year = local.tm_year + 1900, .month = local.tm_mon + 1, .day = local.tm_mday};
}

std::tm noonOf(const Civil& civil, int days)
{
  std::tm local{};
  local.tm_year = civil.year - 1900;
  local.tm_mon = civil.month - 1;
  local.tm_mday = civil.day + days;
  local.tm_hour = kNoon;
  local.tm_isdst = -1;
  std::mktime(&local);
  return local;
}

Civil shifted(const Civil& civil, int days)
{
  const std::tm local = noonOf(civil, days);
  return {.year = local.tm_year + 1900, .month = local.tm_mon + 1, .day = local.tm_mday};
}

int64_t instantOf(const Civil& civil, int minuteOfDay)
{
  std::tm local{};
  local.tm_year = civil.year - 1900;
  local.tm_mon = civil.month - 1;
  local.tm_mday = civil.day;
  local.tm_hour = minuteOfDay / kMinutesInHour;
  local.tm_min = minuteOfDay % kMinutesInHour;
  local.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&local));
}

int daysIn(const Civil& civil)
{
  constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (civil.year % 4 == 0 && civil.year % 100 != 0) || civil.year % 400 == 0;
  return kDays.at(static_cast<std::size_t>(civil.month - 1)) + (civil.month == 2 && leap ? 1 : 0);
}

struct NextInput
{
  const Civil& today;
  int weekday{-1};
  int minuteOfDay{0};
  int64_t now{0};
};

std::optional<int64_t> nextAt(const NextInput& input)
{
  for (int ahead = 0; ahead <= kWeekDays; ++ahead) {
    const std::tm noon = noonOf(input.today, ahead);
    if (input.weekday >= 0 && noon.tm_wday != input.weekday)
      continue;
    const int64_t instant = instantOf(shifted(input.today, ahead), input.minuteOfDay);
    if (instant > input.now)
      return instant;
  }
  return std::nullopt;
}
}

RepeatReading read(const RepeatHeard& heard)
{
  const call_time::Folded folded = call_time::fold(heard.text);
  const Tokens tokens = call_time::tokenize(folded.text);
  if (tokens.empty())
    return {};
  const Found found = Reader(tokens).run();
  if (!found.any)
    return {};
  const bool monthNamed = std::ranges::any_of(tokens, [](const call_time::Token& token) { return call_time::monthOf(token.text) > 0; });
  std::optional<Repeat> repeat = decided(found, monthNamed);
  if (repeat && (repeat->interval < 1 || repeat->interval > kMaxInterval))
    repeat.reset();
  return {.repeat = std::move(repeat),
          .mentioned = true,
          .part = found.part,
          .phraseBegin = folded.origin[tokens[found.first].begin],
          .phraseEnd = folded.origin[tokens[found.last].end]};
}

std::string format(const Repeat& repeat)
{
  std::string out = "FREQ=" + std::string(frequencyName(repeat.frequency));
  if (repeat.interval > 1)
    out += ";INTERVAL=" + std::to_string(repeat.interval);
  if (!repeat.weekdays.empty()) {
    out += ";BYDAY=";
    for (std::size_t index = 0; index < repeat.weekdays.size(); ++index) {
      if (index > 0)
        out += ',';
      out += kByDay.at(static_cast<std::size_t>(repeat.weekdays[index]));
    }
  }
  if (repeat.monthDay > 0)
    out += ";BYMONTHDAY=" + std::to_string(repeat.monthDay);
  return out;
}

std::optional<Repeat> parse(std::string_view text)
{
  if (text.empty() || text.back() == ';')
    return std::nullopt;
  Repeat repeat;
  bool frequencySeen = false;
  bool intervalSeen = false;
  bool daysSeen = false;
  bool monthDaySeen = false;
  while (!text.empty()) {
    const std::size_t end = text.find(';');
    const std::string_view part = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    const std::size_t equals = part.find('=');
    if (equals == std::string_view::npos)
      return std::nullopt;
    const std::string_view key = part.substr(0, equals);
    const std::string_view value = part.substr(equals + 1);
    if (key == "FREQ" && !frequencySeen) {
      const auto frequency = frequencyNamed(value);
      if (!frequency)
        return std::nullopt;
      repeat.frequency = *frequency;
      frequencySeen = true;
    }
    else if (key == "INTERVAL" && !intervalSeen) {
      const auto interval = numberIn({.text = value, .high = kMaxInterval});
      if (!interval)
        return std::nullopt;
      repeat.interval = *interval;
      intervalSeen = true;
    }
    else if (key == "BYDAY" && !daysSeen) {
      auto days = weekdaysIn(value);
      if (!days)
        return std::nullopt;
      repeat.weekdays = std::move(*days);
      daysSeen = true;
    }
    else if (key == "BYMONTHDAY" && !monthDaySeen) {
      const auto day = numberIn({.text = value, .high = kLastMonthDay});
      if (!day)
        return std::nullopt;
      repeat.monthDay = *day;
      monthDaySeen = true;
    }
    else {
      return std::nullopt;
    }
  }
  if (!frequencySeen)
    return std::nullopt;
  if (daysSeen && repeat.frequency != RepeatFrequency::Weekly)
    return std::nullopt;
  if (monthDaySeen && repeat.frequency != RepeatFrequency::Monthly)
    return std::nullopt;
  return repeat;
}

std::string productivityRule(const Repeat& repeat)
{
  std::string out = "FREQ=" + std::string(frequencyName(repeat.frequency));
  if (repeat.interval > 1)
    out += ";INTERVAL=" + std::to_string(repeat.interval);
  return out;
}

std::vector<int64_t> firstOccurrences(const FirstOccurrences& input)
{
  const Civil today = civilOf(input.now);
  const Repeat& repeat = input.repeat;
  if (repeat.frequency == RepeatFrequency::Weekly && !repeat.weekdays.empty()) {
    std::vector<int64_t> firsts;
    firsts.reserve(repeat.weekdays.size());
    for (const int weekday : repeat.weekdays)
      if (const auto first = nextAt({.today = today, .weekday = weekday, .minuteOfDay = input.minuteOfDay, .now = input.now}))
        firsts.push_back(*first);
    std::ranges::sort(firsts);
    return firsts;
  }
  if (repeat.frequency == RepeatFrequency::Monthly && repeat.monthDay > 0) {
    for (int step = 0; step <= kMonthsAhead; ++step) {
      const int index = today.month - 1 + step;
      const Civil civil{.year = today.year + index / kMonthsInYear, .month = index % kMonthsInYear + 1, .day = repeat.monthDay};
      if (civil.day > daysIn(civil))
        continue;
      if (const int64_t instant = instantOf(civil, input.minuteOfDay); instant > input.now)
        return {instant};
    }
    return {};
  }
  if (const auto first = nextAt({.today = today, .weekday = -1, .minuteOfDay = input.minuteOfDay, .now = input.now}))
    return {*first};
  return {};
}

bool namesMonth(std::string_view text)
{
  const call_time::Folded folded = call_time::fold(text);
  const Tokens tokens = call_time::tokenize(folded.text);
  return std::ranges::any_of(tokens, [](const call_time::Token& token) { return call_time::monthOf(token.text) > 0; });
}

}
