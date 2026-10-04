#include "call-time.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <vector>

namespace
{
constexpr int64_t kMinuteS = 60;
constexpr int64_t kHourS = 3600;
constexpr int64_t kMaxAheadS = int64_t{30} * 24 * kHourS;

struct Folded
{
  std::string text;
  std::vector<std::size_t> origin;
};

Folded fold(std::string_view source)
{
  static constexpr std::array<std::pair<std::string_view, char>, 14> kFold{{
      {"á", 'a'}, {"é", 'e'}, {"í", 'i'}, {"ó", 'o'}, {"ú", 'u'},
      {"ü", 'u'}, {"ñ", 'n'}, {"Á", 'a'}, {"É", 'e'}, {"Í", 'i'},
      {"Ó", 'o'}, {"Ú", 'u'}, {"Ü", 'u'}, {"Ñ", 'n'}}};
  Folded folded;
  folded.text.reserve(source.size());
  folded.origin.reserve(source.size() + 1);
  std::size_t index = 0;
  while (index < source.size()) {
    bool matched = false;
    for (const auto& [from, to] : kFold) {
      if (source.substr(index, from.size()) == from) {
        folded.text.push_back(to);
        folded.origin.push_back(index);
        index += from.size();
        matched = true;
        break;
      }
    }
    if (matched)
      continue;
    folded.text.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(source[index]))));
    folded.origin.push_back(index);
    ++index;
  }
  folded.origin.push_back(source.size());
  return folded;
}

struct Token
{
  std::string text;
  std::size_t begin{0};
  std::size_t end{0};
};

std::vector<Token> tokenize(const std::string& text)
{
  std::vector<Token> tokens;
  std::size_t index = 0;
  while (index < text.size()) {
    const auto c = static_cast<unsigned char>(text[index]);
    if (!std::isalnum(c)) {
      ++index;
      continue;
    }
    const std::size_t begin = index;
    while (index < text.size()) {
      const auto d = static_cast<unsigned char>(text[index]);
      if (std::isalnum(d) ||
          (d == ':' && index + 1 < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[index + 1])) &&
           index > begin &&
           std::isdigit(static_cast<unsigned char>(text[index - 1]))))
        ++index;
      else
        break;
    }
    tokens.push_back({.text = text.substr(begin, index - begin),
                      .begin = begin,
                      .end = index});
  }
  return tokens;
}

int smallNumber(const std::string& token)
{
  static constexpr std::array<std::pair<std::string_view, int>, 32> kWords{{
      {"un", 1},     {"una", 1},     {"uno", 1},    {"dos", 2},
      {"tres", 3},   {"cuatro", 4},  {"cinco", 5},  {"seis", 6},
      {"siete", 7},  {"ocho", 8},    {"nueve", 9},  {"diez", 10},
      {"once", 11},  {"doce", 12},   {"quince", 15}, {"veinte", 20},
      {"treinta", 30}, {"a", 1},     {"an", 1},     {"one", 1},
      {"two", 2},    {"three", 3},   {"four", 4},   {"five", 5},
      {"six", 6},    {"seven", 7},   {"eight", 8},  {"nine", 9},
      {"ten", 10},   {"eleven", 11}, {"twelve", 12}, {"fifteen", 15}}};
  for (const auto& [word, value] : kWords) {
    if (token == word)
      return value;
  }
  if (!token.empty() && token.size() <= 3 &&
      std::ranges::all_of(token, [](unsigned char c) { return std::isdigit(c); }))
    return std::stoi(token);
  return -1;
}

struct ClockReading
{
  int hour{-1};
  int minute{0};
  std::size_t endToken{0};
  bool halfKnown{false};
};

std::optional<ClockReading> clockAt(const std::vector<Token>& tokens,
                                    std::size_t index)
{
  if (index >= tokens.size())
    return std::nullopt;
  const std::string& head = tokens[index].text;
  ClockReading reading;
  reading.endToken = index;
  if (const auto colon = head.find(':'); colon != std::string::npos) {
    const int hour = smallNumber(head.substr(0, colon));
    const int minute = smallNumber(head.substr(colon + 1));
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
      return std::nullopt;
    reading.hour = hour;
    reading.minute = minute;
  }
  else if (head == "mediodia" || head == "noon") {
    reading.hour = 12;
  }
  else {
    const int hour = smallNumber(head);
    if (hour < 0 || hour > 23 || head == "a" || head == "an" || head == "un")
      return std::nullopt;
    reading.hour = hour;
  }
  std::size_t next = index + 1;
  const auto word = [&tokens](std::size_t at) {
    return at < tokens.size() ? tokens[at].text : std::string{};
  };
  if (word(next) == "y" && word(next + 1) == "media") {
    reading.minute = 30;
    next += 2;
  }
  else if (word(next) == "y" && word(next + 1) == "cuarto") {
    reading.minute = 15;
    next += 2;
  }
  else if (word(next) == "menos" && word(next + 1) == "cuarto") {
    reading.minute = 45;
    reading.hour = (reading.hour + 23) % 24;
    next += 2;
  }
  const std::string suffix = word(next);
  const bool afternoon = suffix == "pm" ||
                         (suffix == "de" && (word(next + 2) == "tarde" ||
                                             word(next + 2) == "noche"));
  const bool morning = suffix == "am" ||
                       (suffix == "de" && word(next + 2) == "manana");
  reading.halfKnown = afternoon || morning || reading.hour == 0 ||
                      reading.hour >= 12;
  if (afternoon && reading.hour < 12)
    reading.hour += 12;
  if (morning && reading.hour == 12)
    reading.hour = 0;
  if (suffix == "pm" || suffix == "am")
    ++next;
  else if (afternoon || morning)
    next += 3;
  reading.endToken = next - 1;
  return reading;
}

struct LocalTimeInput
{
  int64_t now{0};
  int dayOffset{0};
  int hour{0};
  int minute{0};
};

int64_t atLocal(const LocalTimeInput& input)
{
  const auto seconds = static_cast<std::time_t>(input.now);
  std::tm local{};
  localtime_r(&seconds, &local);
  local.tm_mday += input.dayOffset;
  local.tm_hour = input.hour;
  local.tm_min = input.minute;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&local));
}

int weekdayToday(int64_t now)
{
  const auto seconds = static_cast<std::time_t>(now);
  std::tm local{};
  localtime_r(&seconds, &local);
  return local.tm_wday;
}

int weekdayOf(const std::string& token)
{
  static constexpr std::array<std::string_view, 7> kEs{
      "domingo", "lunes", "martes", "miercoles", "jueves", "viernes", "sabado"};
  static constexpr std::array<std::string_view, 7> kEn{
      "sunday", "monday", "tuesday", "wednesday", "thursday", "friday",
      "saturday"};
  for (std::size_t day = 0; day < kEs.size(); ++day) {
    if (token == kEs[day] || token == kEn[day])
      return static_cast<int>(day);
  }
  return -1;
}

struct Span
{
  std::size_t begin{0};
  std::size_t end{0};
};

Span spanOf(const std::vector<Token>& tokens, std::size_t first, std::size_t last)
{
  return {.begin = tokens[first].begin, .end = tokens[last].end};
}

struct Found
{
  int64_t fireAt{0};
  Span span;
};

std::optional<Found> relative(const std::vector<Token>& tokens, int64_t now)
{
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const std::string& word = tokens[index].text;
    std::size_t start = index;
    std::size_t at = index;
    if (word == "dentro" && index + 1 < tokens.size() &&
        tokens[index + 1].text == "de")
      at = index + 2;
    else if (word == "en" || word == "in")
      at = index + 1;
    else
      continue;
    if (at >= tokens.size())
      continue;
    if (tokens[at].text == "media" && at + 1 < tokens.size() &&
        tokens[at + 1].text == "hora")
      return Found{.fireAt = now + 30 * kMinuteS,
                   .span = spanOf(tokens, start, at + 1)};
    if (tokens[at].text == "half" && at + 2 < tokens.size() &&
        tokens[at + 1].text == "an" && tokens[at + 2].text == "hour")
      return Found{.fireAt = now + 30 * kMinuteS,
                   .span = spanOf(tokens, start, at + 2)};
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
    return Found{.fireAt = now + amount * step,
                 .span = spanOf(tokens, start, at + 1)};
  }
  return std::nullopt;
}
}

std::optional<CallTime> call_time::resolve(const CallTimeInput& input)
{
  const Folded folded = fold(input.text);
  const auto tokens = tokenize(folded.text);
  if (tokens.empty())
    return std::nullopt;
  const auto toCallTime = [&folded](const Found& found) {
    return CallTime{.fireAt = found.fireAt,
                    .phraseBegin = folded.origin[found.span.begin],
                    .phraseEnd = folded.origin[found.span.end]};
  };
  if (const auto found = relative(tokens, input.now)) {
    if (found->fireAt - input.now <= kMaxAheadS)
      return toCallTime(*found);
    return std::nullopt;
  }

  int dayOffset = 0;
  bool dayGiven = false;
  std::size_t phraseFirst = tokens.size();
  std::size_t phraseLast = 0;
  const auto widen = [&phraseFirst, &phraseLast](std::size_t first,
                                                 std::size_t last) {
    phraseFirst = std::min(phraseFirst, first);
    phraseLast = std::max(phraseLast, last);
  };
  std::optional<ClockReading> clock;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const std::string& word = tokens[index].text;
    if (word == "pasado" && index + 1 < tokens.size() &&
        tokens[index + 1].text == "manana") {
      dayOffset = 2;
      dayGiven = true;
      widen(index, index + 1);
      ++index;
      continue;
    }
    const bool partOfDay = index > 0 && tokens[index - 1].text == "la" &&
                           index > 1 && tokens[index - 2].text == "de";
    if ((word == "manana" && !partOfDay) || word == "tomorrow") {
      dayOffset = 1;
      dayGiven = true;
      widen(index, index);
      continue;
    }
    if (word == "hoy" || word == "today" || word == "tonight") {
      dayOffset = 0;
      dayGiven = true;
      widen(index, index);
      continue;
    }
    if (const int weekday = weekdayOf(word); weekday >= 0) {
      const int today = weekdayToday(input.now);
      dayOffset = (weekday - today + 7) % 7;
      if (dayOffset == 0)
        dayOffset = 7;
      dayGiven = true;
      const bool article = index > 0 && (tokens[index - 1].text == "el" ||
                                         tokens[index - 1].text == "on");
      widen(article ? index - 1 : index, index);
      continue;
    }
    const bool spanishClock =
        (word == "las" || word == "la") && index > 0 && tokens[index - 1].text == "a";
    if ((spanishClock || word == "at") && !clock) {
      if (auto reading = clockAt(tokens, index + 1)) {
        clock = reading;
        widen(spanishClock ? index - 1 : index, reading->endToken);
        index = reading->endToken;
      }
    }
  }
  if (!clock)
    return std::nullopt;
  int64_t fireAt = atLocal({.now = input.now,
                            .dayOffset = dayOffset,
                            .hour = clock->hour,
                            .minute = clock->minute});
  if (fireAt <= input.now && !dayGiven && !clock->halfKnown) {
    const int64_t evening =
        atLocal({.now = input.now,
                 .dayOffset = 0,
                 .hour = clock->hour + 12,
                 .minute = clock->minute});
    if (evening > input.now)
      fireAt = evening;
  }
  if (fireAt <= input.now) {
    if (dayGiven)
      return std::nullopt;
    fireAt = atLocal({.now = input.now,
                      .dayOffset = 1,
                      .hour = clock->hour,
                      .minute = clock->minute});
  }
  if (fireAt - input.now > kMaxAheadS)
    return std::nullopt;
  return toCallTime(
      Found{.fireAt = fireAt, .span = spanOf(tokens, phraseFirst, phraseLast)});
}

std::string call_time::withoutPhrase(std::string_view text,
                                     const CallTime& time)
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
  while (!compact.empty() &&
         (compact.back() == ',' || compact.back() == '.' || compact.back() == ' '))
    compact.pop_back();
  return compact;
}
