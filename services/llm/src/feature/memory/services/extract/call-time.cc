#include "call-time.hxx"

#include "call-time-words.hxx"

#include <algorithm>
#include <array>
#include <ctime>
#include <limits>

namespace call_time
{

namespace
{
constexpr int64_t kMinuteS = 60;
constexpr int64_t kHourS = 3600;
constexpr int64_t kRelativeAheadS = int64_t{30} * 24 * kHourS;
constexpr int kMonthsAhead = 12;
constexpr int kMonthsInYear = 12;
constexpr int kNoon = 12;
constexpr int kEndOfDay = 24;
constexpr int kSmallHours = 5;
constexpr int kAfternoonHours = 6;
constexpr int kWeekDays = 7;

enum class Period : unsigned char
{
  None,
  Am,
  Pm,
  Morning,
  Afternoon,
  Night,
  Dawn
};

struct Marker
{
  Period period{Period::None};
  std::size_t length{0};
  bool today{false};
};

struct ClockReading
{
  int hour{0};
  int minute{0};
  int minus{0};
  Period period{Period::None};
  bool literal{false};
  bool midday{false};
  bool midnight{false};
  std::size_t last{0};
};

struct DateHit
{
  int day{0};
  int month{0};
};

struct Parsed
{
  int offset{-1};
  bool today{false};
  int weekday{-1};
  std::optional<DateHit> date;
  std::optional<ClockReading> clock;
  Period context{Period::None};
  std::size_t first{std::numeric_limits<std::size_t>::max()};
  std::size_t last{0};
};

struct Civil
{
  int year{0};
  int month{0};
  int day{0};
};

int daysInMonth(const Civil& civil)
{
  constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (civil.year % 4 == 0 && civil.year % 100 != 0) || civil.year % 400 == 0;
  return kDays.at(static_cast<std::size_t>(civil.month - 1)) + (civil.month == 2 && leap ? 1 : 0);
}

bool isValid(const Civil& civil)
{
  return civil.month >= 1 && civil.month <= kMonthsInYear && civil.day >= 1 && civil.day <= daysInMonth(civil);
}

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

int weekdayIndex(const Civil& civil)
{
  return noonOf(civil, 0).tm_wday;
}

int hour24(const ClockReading& reading)
{
  if (reading.midday)
    return kNoon;
  if (reading.midnight)
    return kEndOfDay;
  if (reading.literal)
    return reading.hour;
  const int hour = reading.hour;
  switch (reading.period) {
    case Period::Am:
    case Period::Dawn:
      return hour % kNoon;
    case Period::Pm:
    case Period::Afternoon:
      return hour % kNoon + kNoon;
    case Period::Morning:
      return hour;
    case Period::Night:
      if (hour == kNoon)
        return kEndOfDay;
      return hour <= kSmallHours ? hour : hour + kNoon;
    case Period::None:
      break;
  }
  if (hour == kNoon)
    return kNoon;
  return hour <= kAfternoonHours ? hour + kNoon : hour;
}

int64_t instantOf(const Civil& civil, const ClockReading& reading)
{
  std::tm local{};
  local.tm_year = civil.year - 1900;
  local.tm_mon = civil.month - 1;
  local.tm_mday = civil.day;
  local.tm_hour = hour24(reading);
  local.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&local)) + (reading.minute - reading.minus) * kMinuteS;
}

int64_t yearAfter(int64_t epoch)
{
  const auto seconds = static_cast<std::time_t>(epoch);
  std::tm local{};
  localtime_r(&seconds, &local);
  local.tm_year += 1;
  local.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&local));
}

bool onlyDigits(const std::string& text)
{
  return !text.empty() && std::ranges::all_of(text, [](unsigned char c) { return c >= '0' && c <= '9'; });
}

Period spanishPart(const Tokens& tokens, std::size_t at)
{
  if (wordAt(tokens, at, "manana"))
    return Period::Morning;
  if (wordAt(tokens, at, "tarde"))
    return Period::Afternoon;
  if (wordAt(tokens, at, "noche"))
    return Period::Night;
  if (wordAt(tokens, at, "madrugada"))
    return Period::Dawn;
  return Period::None;
}

Period englishPart(const Tokens& tokens, std::size_t at)
{
  if (wordAt(tokens, at, "morning"))
    return Period::Morning;
  if (anyWordAt(tokens, at, {"afternoon", "evening"}))
    return Period::Afternoon;
  if (wordAt(tokens, at, "night"))
    return Period::Night;
  return Period::None;
}

std::optional<Marker> strictMarkerAt(const Tokens& tokens, std::size_t at)
{
  if (wordAt(tokens, at, "am"))
    return Marker{.period = Period::Am, .length = 1, .today = false};
  if (wordAt(tokens, at, "pm"))
    return Marker{.period = Period::Pm, .length = 1, .today = false};
  if (wordAt(tokens, at, "a") && wordAt(tokens, at + 1, "m"))
    return Marker{.period = Period::Am, .length = 2, .today = false};
  if (wordAt(tokens, at, "p") && wordAt(tokens, at + 1, "m"))
    return Marker{.period = Period::Pm, .length = 2, .today = false};
  return std::nullopt;
}

std::optional<Marker> phraseMarkerAt(const Tokens& tokens, std::size_t at)
{
  if (anyWordAt(tokens, at, {"de", "en", "por"}) && wordAt(tokens, at + 1, "la")) {
    if (const Period period = spanishPart(tokens, at + 2); period != Period::None)
      return Marker{.period = period, .length = 3, .today = false};
  }
  if (wordAt(tokens, at, "esta")) {
    if (const Period period = spanishPart(tokens, at + 1); period != Period::None)
      return Marker{.period = period, .length = 2, .today = true};
  }
  if (wordAt(tokens, at, "del") && wordAt(tokens, at + 1, "mediodia"))
    return Marker{.period = Period::Afternoon, .length = 2, .today = false};
  if (wordAt(tokens, at, "in") && wordAt(tokens, at + 1, "the")) {
    if (const Period period = englishPart(tokens, at + 2); period != Period::None)
      return Marker{.period = period, .length = 3, .today = false};
  }
  if (wordAt(tokens, at, "at") && wordAt(tokens, at + 1, "night"))
    return Marker{.period = Period::Night, .length = 2, .today = false};
  if (wordAt(tokens, at, "this")) {
    if (const Period period = englishPart(tokens, at + 1); period != Period::None)
      return Marker{.period = period, .length = 2, .today = true};
  }
  if (wordAt(tokens, at, "tonight"))
    return Marker{.period = Period::Night, .length = 1, .today = true};
  if (const Period period = englishPart(tokens, at); period != Period::None)
    return Marker{.period = period, .length = 1, .today = false};
  return std::nullopt;
}

bool clockMarkerAt(const Tokens& tokens, std::size_t at)
{
  return strictMarkerAt(tokens, at) || phraseMarkerAt(tokens, at) || anyWordAt(tokens, at, {"y", "menos"}) ||
         (wordAt(tokens, at, "en") && wordAt(tokens, at + 1, "punto"));
}

std::optional<ClockReading> clockAt(const Tokens& tokens, std::size_t at)
{
  if (at >= tokens.size())
    return std::nullopt;
  const std::string& head = tokens[at].text;
  ClockReading reading;
  bool spelled = false;
  if (const auto colon = head.find(':'); colon != std::string::npos) {
    const std::string hours = head.substr(0, colon);
    const std::string minutes = head.substr(colon + 1);
    if (hours.size() > 2 || minutes.size() != 2)
      return std::nullopt;
    reading.hour = std::stoi(hours);
    reading.minute = std::stoi(minutes);
    if (reading.hour > 23 || reading.minute > 59)
      return std::nullopt;
    reading.literal = reading.hour == 0 || reading.hour >= kNoon + 1 || (hours.size() == 2 && hours.front() == '0');
  }
  else if (head == "mediodia" || head == "noon" || head == "midday") {
    reading.midday = true;
    reading.hour = kNoon;
  }
  else if (head == "medianoche" || head == "midnight") {
    reading.midnight = true;
  }
  else {
    const int hour = smallNumber(head);
    if (hour < 0 || hour > 23 || head == "a" || head == "an" || head == "un")
      return std::nullopt;
    reading.hour = hour;
    spelled = !onlyDigits(head);
    reading.literal = hour == 0 || hour >= kNoon + 1 || (onlyDigits(head) && head.size() == 2 && head.front() == '0');
  }
  std::size_t next = at + 1;
  if (spelled && wordAt(tokens, next, "hundred")) {
    ++next;
    if (wordAt(tokens, next, "hours"))
      ++next;
  }
  else if (!reading.midday && !reading.midnight) {
    if (wordAt(tokens, next, "y")) {
      if (const auto minutes = minutesAt(tokens, next + 1)) {
        reading.minute = minutes->value;
        next += 1 + minutes->length;
      }
    }
    else if (wordAt(tokens, next, "menos")) {
      if (const auto minutes = minutesAt(tokens, next + 1); minutes && minutes->value != 30) {
        reading.minus = minutes->value;
        next += 1 + minutes->length;
      }
    }
    else if (spelled) {
      if (const auto minutes = englishMinutesAt(tokens, next)) {
        reading.minute = minutes->value;
        next += minutes->length;
      }
    }
  }
  if ((wordAt(tokens, next, "o") && wordAt(tokens, next + 1, "clock")) || (wordAt(tokens, next, "en") && wordAt(tokens, next + 1, "punto")))
    next += 2;
  else if (wordAt(tokens, next, "sharp"))
    next += 1;
  if (const auto strict = strictMarkerAt(tokens, next)) {
    reading.period = strict->period;
    next += strict->length;
  }
  else if (const auto phrase = phraseMarkerAt(tokens, next)) {
    reading.period = phrase->period;
    next += phrase->length;
  }
  reading.last = next - 1;
  return reading;
}

std::optional<ClockReading> pastAt(const Tokens& tokens, std::size_t at)
{
  int minutes = 0;
  std::size_t next = at;
  if (wordAt(tokens, at, "half")) {
    minutes = 30;
    next = at + 1;
  }
  else if (wordAt(tokens, at, "quarter")) {
    minutes = 15;
    next = at + 1;
  }
  else if (const auto spoken = englishMinutesAt(tokens, at); spoken && spoken->value <= 25) {
    minutes = spoken->value;
    next = at + spoken->length;
  }
  else {
    return std::nullopt;
  }
  const bool past = wordAt(tokens, next, "past");
  if (!past && !wordAt(tokens, next, "to"))
    return std::nullopt;
  auto reading = clockAt(tokens, next + 1);
  if (!reading || reading->literal)
    return std::nullopt;
  if (past)
    reading->minute += minutes;
  else
    reading->minus += minutes;
  return reading;
}

std::optional<Counted> relativeAmountAt(const Tokens& tokens, std::size_t at)
{
  if (wordAt(tokens, at, "media") && wordAt(tokens, at + 1, "hora"))
    return Counted{.value = 30, .length = 2};
  if (wordAt(tokens, at, "half") && wordAt(tokens, at + 1, "an") && wordAt(tokens, at + 2, "hour"))
    return Counted{.value = 30, .length = 3};
  return std::nullopt;
}

struct Relative
{
  int64_t fireAt{0};
  std::size_t first{0};
  std::size_t last{0};
};

std::optional<Relative> relative(const Tokens& tokens, int64_t now)
{
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    std::size_t at = index;
    if (wordAt(tokens, index, "dentro") && wordAt(tokens, index + 1, "de"))
      at = index + 2;
    else if (anyWordAt(tokens, index, {"en", "in"}))
      at = index + 1;
    else
      continue;
    if (at >= tokens.size())
      continue;
    if (const auto half = relativeAmountAt(tokens, at))
      return Relative{.fireAt = now + half->value * kMinuteS, .first = index, .last = at + half->length - 1};
    const int amount = smallNumber(tokens[at].text);
    if (amount <= 0 || at + 1 >= tokens.size())
      continue;
    const std::string& unit = tokens[at + 1].text;
    int64_t step = 0;
    if (unit.starts_with("minuto") || unit.starts_with("minute") || unit == "min")
      step = kMinuteS;
    else if (unit.starts_with("hora") || unit.starts_with("hour"))
      step = kHourS;
    if (step == 0)
      continue;
    return Relative{.fireAt = now + amount * step, .first = index, .last = at + 1};
  }
  return std::nullopt;
}

class Scanner
{
public:
  explicit Scanner(const Tokens& tokens) : tokens_(tokens) {}

  [[nodiscard]] Parsed run()
  {
    std::size_t index = 0;
    while (index < tokens_.size()) {
      std::size_t used = marker(index);
      if (used == 0)
        used = day(index);
      if (used == 0)
        used = weekday(index);
      if (used == 0)
        used = date(index);
      if (used == 0)
        used = clock(index);
      index += std::max<std::size_t>(used, 1);
    }
    return parsed_;
  }

private:
  void widen(std::size_t first, std::size_t last)
  {
    parsed_.first = std::min(parsed_.first, first);
    parsed_.last = std::max(parsed_.last, last);
  }

  std::size_t marker(std::size_t at)
  {
    const auto found = phraseMarkerAt(tokens_, at);
    if (!found)
      return 0;
    if (parsed_.context == Period::None)
      parsed_.context = found->period;
    if (found->today) {
      parsed_.today = true;
      if (parsed_.offset < 0)
        parsed_.offset = 0;
    }
    widen(at, at + found->length - 1);
    return found->length;
  }

  std::size_t day(std::size_t at)
  {
    std::size_t length = 0;
    int offset = 0;
    bool today = false;
    if (wordAt(tokens_, at, "pasado") && wordAt(tokens_, at + 1, "manana")) {
      offset = 2;
      length = 2;
    }
    else if (wordAt(tokens_, at, "day") && wordAt(tokens_, at + 1, "after") && wordAt(tokens_, at + 2, "tomorrow")) {
      offset = 2;
      length = 3;
    }
    else if (anyWordAt(tokens_, at, {"manana", "tomorrow"})) {
      offset = 1;
      length = 1;
    }
    else if (anyWordAt(tokens_, at, {"hoy", "today"})) {
      today = true;
      length = 1;
    }
    if (length == 0)
      return 0;
    if (parsed_.offset < 0)
      parsed_.offset = offset;
    parsed_.today = parsed_.today || today;
    widen(at, at + length - 1);
    return length;
  }

  std::optional<DateHit> monthAfter(std::size_t at, DateHit hit, std::size_t& end) const
  {
    std::size_t next = at;
    if (anyWordAt(tokens_, next, {"de", "del", "of"}))
      ++next;
    if (next < tokens_.size()) {
      if (const int month = monthOf(tokens_[next].text); month > 0) {
        hit.month = month;
        end = next;
        return hit;
      }
    }
    return std::nullopt;
  }

  std::size_t weekday(std::size_t at)
  {
    const int found = weekdayOf(tokens_[at].text);
    if (found < 0)
      return 0;
    parsed_.weekday = found;
    const bool article = at > 0 && anyWordAt(tokens_, at - 1, {"el", "on"});
    widen(article ? at - 1 : at, at);
    std::size_t next = at + 1;
    if (wordAt(tokens_, next, "the"))
      ++next;
    const auto number = dayNumberAt(tokens_, next);
    if (!number || parsed_.date)
      return 1;
    const std::size_t after = next + number->length;
    if (strictMarkerAt(tokens_, after) || clockMarkerAt(tokens_, after))
      return 1;
    DateHit hit{.day = number->value, .month = 0};
    std::size_t end = after - 1;
    if (const auto withMonth = monthAfter(after, hit, end))
      hit = *withMonth;
    parsed_.date = hit;
    widen(at, end);
    return end - at + 1;
  }

  std::size_t date(std::size_t at)
  {
    if (parsed_.date)
      return 0;
    const bool spanish = wordAt(tokens_, at, "el");
    const bool english = wordAt(tokens_, at, "the");
    if (spanish || english) {
      std::size_t next = at + 1;
      if (spanish && wordAt(tokens_, next, "dia"))
        ++next;
      const auto number = dayNumberAt(tokens_, next);
      if (!number || (english && !number->ordinal))
        return 0;
      const std::size_t after = next + number->length;
      if (strictMarkerAt(tokens_, after) || clockMarkerAt(tokens_, after))
        return 0;
      DateHit hit{.day = number->value, .month = 0};
      std::size_t end = after - 1;
      if (const auto withMonth = monthAfter(after, hit, end))
        hit = *withMonth;
      parsed_.date = hit;
      const bool lead = at > 0 && wordAt(tokens_, at - 1, "on");
      widen(lead ? at - 1 : at, end);
      return end - at + 1;
    }
    if (const int month = monthOf(tokens_[at].text); month > 0) {
      auto number = dayNumberAt(tokens_, at + 1);
      if (!number)
        number = cardinalDayAt(tokens_, at + 1);
      if (!number || (!onlyDigits(tokens_[at + 1].text) && !number->ordinal))
        return 0;
      if (strictMarkerAt(tokens_, at + 1 + number->length))
        return 0;
      parsed_.date = DateHit{.day = number->value, .month = month};
      widen(at, at + number->length);
      return number->length + 1;
    }
    if (const auto number = dayNumberAt(tokens_, at)) {
      std::size_t end = at + number->length - 1;
      DateHit hit{.day = number->value, .month = 0};
      if (const auto withMonth = monthAfter(at + number->length, hit, end)) {
        parsed_.date = *withMonth;
        widen(at, end);
        return end - at + 1;
      }
    }
    return 0;
  }

  void take(const ClockReading& reading, std::size_t first)
  {
    parsed_.clock = reading;
    widen(first, reading.last);
  }

  std::size_t clock(std::size_t at)
  {
    if (parsed_.clock)
      return 0;
    const bool introduced = (anyWordAt(tokens_, at, {"las", "la"}) && at > 0 && anyWordAt(tokens_, at - 1, {"a", "para", "hacia", "sobre"})) ||
                            (wordAt(tokens_, at, "las") && at > 1 && wordAt(tokens_, at - 1, "de") && wordAt(tokens_, at - 2, "eso"));
    if (introduced) {
      if (const auto reading = clockAt(tokens_, at + 1)) {
        take(*reading, at - 1);
        return reading->last - at + 1;
      }
      return 0;
    }
    if (wordAt(tokens_, at, "at")) {
      if (const auto reading = pastAt(tokens_, at + 1)) {
        take(*reading, at);
        return reading->last - at + 1;
      }
      if (const auto reading = clockAt(tokens_, at + 1)) {
        take(*reading, at);
        return reading->last - at + 1;
      }
      return 0;
    }
    if (const auto reading = pastAt(tokens_, at)) {
      take(*reading, at);
      return reading->last - at + 1;
    }
    const std::string& head = tokens_[at].text;
    const bool keyword = anyWordAt(tokens_, at, {"mediodia", "noon", "midday", "medianoche", "midnight"});
    const bool colon = head.find(':') != std::string::npos;
    const bool strict = (smallNumber(head) >= 0 && head != "a" && head != "an" && head != "un") &&
                        (strictMarkerAt(tokens_, at + 1) || (wordAt(tokens_, at + 1, "o") && wordAt(tokens_, at + 2, "clock")));
    if (!keyword && !colon && !strict)
      return 0;
    if (const auto reading = clockAt(tokens_, at)) {
      const bool lead = keyword && at > 0 && anyWordAt(tokens_, at - 1, {"a", "al", "at", "del"});
      take(*reading, lead ? at - 1 : at);
      return reading->last - at + 1;
    }
    return 0;
  }

  const Tokens& tokens_;
  Parsed parsed_;
};

std::optional<Civil> upcomingDate(const DateHit& hit, const Civil& today, const ClockReading& clock, int64_t now)
{
  const auto ahead = [&](const Civil& civil) { return isValid(civil) && instantOf(civil, clock) > now; };
  if (hit.month > 0) {
    for (int year = today.year; year <= today.year + 1; ++year)
      if (const Civil civil{.year = year, .month = hit.month, .day = hit.day}; ahead(civil))
        return civil;
    return std::nullopt;
  }
  for (int step = 0; step <= kMonthsAhead; ++step) {
    const int index = today.month - 1 + step;
    const Civil civil{.year = today.year + index / kMonthsInYear, .month = index % kMonthsInYear + 1, .day = hit.day};
    if (ahead(civil))
      return civil;
  }
  return std::nullopt;
}

struct Resolved
{
  std::optional<int64_t> fireAt;
  std::optional<DayConflict> conflict;
};

struct Disagreement
{
  Civil date;
  int weekday{0};
  const ClockReading& clock;
  int64_t now{0};
};

DayConflict conflictOf(const Disagreement& input)
{
  const int forward = (input.weekday - weekdayIndex(input.date) + kWeekDays) % kWeekDays;
  const int backward = kWeekDays - forward;
  Civil day = shifted(input.date, forward <= backward ? forward : -backward);
  if (instantOf(day, input.clock) <= input.now)
    day = shifted(day, kWeekDays);
  return {.byWeekday = instantOf(day, input.clock), .byDate = instantOf(input.date, input.clock), .phraseBegin = 0, .phraseEnd = 0};
}

Resolved instantFor(const Parsed& parsed, ClockReading clock, int64_t now)
{
  if (clock.period == Period::None)
    clock.period = parsed.context;
  const Civil today = civilOf(now);
  if (parsed.date) {
    const auto date = upcomingDate(*parsed.date, today, clock, now);
    if (!date)
      return {};
    if (parsed.weekday >= 0 && weekdayIndex(*date) != parsed.weekday) {
      return {.fireAt = std::nullopt, .conflict = conflictOf({.date = *date, .weekday = parsed.weekday, .clock = clock, .now = now})};
    }
    return {.fireAt = instantOf(*date, clock), .conflict = std::nullopt};
  }
  if (parsed.weekday >= 0) {
    int ahead = parsed.offset >= 0 ? parsed.offset : (parsed.weekday - weekdayIndex(today) + kWeekDays) % kWeekDays;
    if (parsed.offset < 0 && ahead == 0)
      ahead = kWeekDays;
    const Civil day = shifted(today, ahead);
    if (weekdayIndex(day) != parsed.weekday)
      return {};
    const int64_t fireAt = instantOf(day, clock);
    if (fireAt <= now)
      return {};
    return {.fireAt = fireAt, .conflict = std::nullopt};
  }
  if (parsed.offset > 0)
    return {.fireAt = instantOf(shifted(today, parsed.offset), clock), .conflict = std::nullopt};
  int64_t fireAt = instantOf(today, clock);
  if (fireAt > now)
    return {.fireAt = fireAt, .conflict = std::nullopt};
  if (parsed.offset == 0 || parsed.today)
    return {};
  fireAt = instantOf(shifted(today, 1), clock);
  return {.fireAt = fireAt, .conflict = std::nullopt};
}
}

CallReading read(const CallTimeInput& input)
{
  const Folded folded = fold(input.text);
  const Tokens tokens = tokenize(folded.text);
  if (tokens.empty())
    return {};
  const auto timeOf = [&](int64_t fireAt, std::size_t first, std::size_t last) {
    return CallTime{.fireAt = fireAt, .phraseBegin = folded.origin[tokens[first].begin], .phraseEnd = folded.origin[tokens[last].end]};
  };
  if (const auto found = relative(tokens, input.now)) {
    if (found->fireAt - input.now > kRelativeAheadS)
      return {};
    return {.time = timeOf(found->fireAt, found->first, found->last), .conflict = std::nullopt};
  }
  const Parsed parsed = Scanner(tokens).run();
  if (!parsed.clock)
    return {};
  const Resolved result = instantFor(parsed, *parsed.clock, input.now);
  const int64_t limit = yearAfter(input.now);
  if (result.conflict) {
    if (std::max(result.conflict->byWeekday, result.conflict->byDate) > limit)
      return {};
    DayConflict conflict = *result.conflict;
    conflict.phraseBegin = folded.origin[tokens[parsed.first].begin];
    conflict.phraseEnd = folded.origin[tokens[parsed.last].end];
    return {.time = std::nullopt, .conflict = conflict};
  }
  if (!result.fireAt || *result.fireAt > limit)
    return {};
  return {.time = timeOf(*result.fireAt, parsed.first, parsed.last), .conflict = std::nullopt};
}

std::optional<CallTime> resolve(const CallTimeInput& input)
{
  return read(input).time;
}

std::string withoutPhrase(std::string_view text, const CallTime& time)
{
  if (time.phraseEnd <= time.phraseBegin || time.phraseEnd > text.size())
    return std::string(text);
  std::string out(text.substr(0, time.phraseBegin));
  out += text.substr(time.phraseEnd);
  std::string compact;
  compact.reserve(out.size());
  bool space = false;
  for (const char c : out) {
    if (c == ' ' || (compact.empty() && (c == ',' || c == '.'))) {
      space = !compact.empty();
      continue;
    }
    if (space && c != ',' && c != '.')
      compact.push_back(' ');
    space = false;
    compact.push_back(c);
  }
  while (!compact.empty() && (compact.back() == ',' || compact.back() == '.' || compact.back() == ' '))
    compact.pop_back();
  return compact;
}

}
