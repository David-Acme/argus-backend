#include "temporal-resolver.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

std::string foldAccents(std::string_view s)
{
  static constexpr std::array<std::pair<std::string_view, char>, 23> kFold{{
      {"á", 'a'}, {"é", 'e'},
      {"í", 'i'}, {"ó", 'o'}, {"ú", 'u'}, {"ü", 'u'}, {"ñ", 'n'},
      {"à", 'a'}, {"è", 'e'}, {"ì", 'i'}, {"ò", 'o'}, {"ù", 'u'},
      {"â", 'a'}, {"ê", 'e'}, {"î", 'i'}, {"ô", 'o'}, {"û", 'u'},
      {"ä", 'a'}, {"ë", 'e'}, {"ï", 'i'}, {"ö", 'o'}, {"ÿ", 'y'},
      {"ç", 'c'}}};
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    bool folded = false;
    for (const auto& [from, to] : kFold) {
      if (s.substr(i, from.size()) == from) {
        out.push_back(to);
        i += from.size();
        folded = true;
        break;
      }
    }
    if (!folded) {
      out.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
      ++i;
    }
  }
  return out;
}

std::vector<std::string> words(const std::string& text)
{
  std::vector<std::string> out;
  std::string cur;
  for (const char c : text) {
    if (c == ' ') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    if (std::isalnum(static_cast<unsigned char>(c)))
      cur.push_back(c);
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

} // namespace

std::string TemporalResolver::normalize(std::string_view text)
{
  std::string folded = foldAccents(text);
  std::string out;
  out.reserve(folded.size());
  bool pendingSpace = false;
  for (const char c : folded) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      pendingSpace = true;
      continue;
    }
    if (pendingSpace && !out.empty())
      out.push_back(' ');
    pendingSpace = false;
    out.push_back(c);
  }
  return out;
}

int TemporalResolver::weekdayFor(std::string_view lang,
                                 std::string_view token) const
{
  static constexpr std::array<std::string_view, 7> kEs{
      "domingo", "lunes", "martes", "miercoles", "jueves", "viernes",
      "sabado"};
  static constexpr std::array<std::string_view, 7> kEn{
      "sunday", "monday", "tuesday", "wednesday", "thursday", "friday",
      "saturday"};
  const bool es = lang == "es";
  const auto& table = es ? kEs : kEn;
  constexpr size_t count = kEs.size();
  for (size_t i = 0; i < count; ++i) {
    if (token == table[i])
      return static_cast<int>(i);
  }
  std::string singular(token);
  if (singular.size() > (es ? 4 : 3) && singular.back() == 's')
    singular.pop_back();
  for (size_t i = 0; i < count; ++i) {
    if (singular == table[i])
      return static_cast<int>(i);
  }
  if (!es) {
    for (size_t i = 0; i < count; ++i) {
      if (token == kEs[i])
        return static_cast<int>(i);
    }
  }
  return -1;
}

int TemporalResolver::hourFor(std::string_view token) const
{
  static constexpr std::array<std::string_view, 26> kWords{
      "una", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho",
      "nueve", "diez", "once", "doce", "one", "two", "three", "four",
      "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
      "mediodia", "noon"};
  for (size_t i = 0; i < kWords.size(); ++i) {
    if (token == kWords[i]) {
      if (i < 12)
        return (i == 0) ? 1 : static_cast<int>(i);
      if (i < 24)
        return static_cast<int>(i - 12 + 1);
      return 12;
    }
  }
  const std::string t(token);
  const bool allDigits =
      !t.empty() && std::all_of(t.begin(), t.end(), [](unsigned char c) {
        return std::isdigit(c);
      });
  if (allDigits) {
    const int digit = std::atoi(t.c_str());
    if (digit >= 0 && digit <= 12)
      return digit;
  }
  return -1;
}

void TemporalResolver::resolve(std::string_view lang,
                               const std::string& normalized,
                               extract::TemporalValue& out) const
{
  out.surface = normalized;
  const auto tokens = words(normalized);
  const bool es = lang == "es";

  auto findToken = [&](const char* a, const char* b) {
    for (const auto& t : tokens)
      if (t == a || t == b)
        return true;
    return false;
  };

  if (tokens.empty())
    return;

  const std::string& first = tokens.front();
  if (first == "manana" || first == "hoy" || first == "ayer" ||
      first == "tomorrow" || first == "today" || first == "yesterday") {
    out.kind = extract::TemporalKind::Relative;
    return;
  }

  if (findToken("cada", "every")) {
    if (findToken("mes", "month") || findToken("meses", "months")) {
      out.kind = extract::TemporalKind::Recurrence;
      out.recur = extract::Recurrence::Monthly;
      return;
    }
    if (findToken("manana", "morning") || findToken("tarde", "afternoon") ||
        findToken("noche", "night") || findToken("dia", "day")) {
      out.kind = extract::TemporalKind::Recurrence;
      out.recur = extract::Recurrence::Daily;
      return;
    }
    for (const auto& t : tokens) {
      const int wd = weekdayFor(lang, t);
      if (wd >= 0) {
        out.kind = extract::TemporalKind::Recurrence;
        out.recur = extract::Recurrence::Weekly;
        out.weekday = wd;
        return;
      }
    }
    out.kind = extract::TemporalKind::Recurrence;
    out.recur = extract::Recurrence::Weekly;
    return;
  }

  if (findToken("los", "on")) {
    for (const auto& t : tokens) {
      const int wd = weekdayFor(lang, t);
      if (wd >= 0) {
        out.kind = extract::TemporalKind::Recurrence;
        out.recur = extract::Recurrence::Weekly;
        out.weekday = wd;
        return;
      }
    }
  }

  for (const auto& t : tokens) {
    const int wd = weekdayFor(lang, t);
    if (wd >= 0) {
      const bool plural = t.size() > 3 && t.back() == 's' &&
                          weekdayFor(lang, t.substr(0, t.size() - 1)) >= 0;
      out.kind = plural ? extract::TemporalKind::Recurrence
                        : extract::TemporalKind::Weekday;
      out.weekday = wd;
      if (plural)
        out.recur = extract::Recurrence::Weekly;
      return;
    }
  }

  for (size_t i = 0; i < tokens.size(); ++i) {
    const int hour = hourFor(tokens[i]);
    if (hour >= 0) {
      int minute = hour * 60;
      if (i + 1 < tokens.size() && tokens[i + 1] == "de" &&
          i + 2 < tokens.size()) {
        const std::string& part = tokens[i + 2];
        if ((es && part == "noche") || (!es && part == "night"))
          minute = (hour < 8 ? hour + 12 : hour) * 60;
        else if ((es && part == "tarde") || (!es && part == "afternoon"))
          minute = (hour < 8 ? hour + 12 : hour) * 60;
      }
      out.kind = extract::TemporalKind::Time;
      out.minuteOfDay = minute;
      return;
    }
  }

  static constexpr std::array<std::string_view, 24> kMonths{
      "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
      "agosto", "septiembre", "octubre", "noviembre", "diciembre",
      "january", "february", "march", "april", "may", "june", "july",
      "august", "september", "october", "november", "december"};
  static constexpr std::array<std::string_view, 9> kSeasons{
      "invierno", "primavera", "verano", "otono", "winter", "spring",
      "summer", "autumn", "fall"};
  for (const auto& t : tokens) {
    for (const std::string_view m : kMonths) {
      if (t == m) {
        out.kind = extract::TemporalKind::Relative;
        return;
      }
    }
    for (const std::string_view s2 : kSeasons) {
      if (t == s2) {
        out.kind = extract::TemporalKind::Relative;
        return;
      }
    }
  }

  out.kind = extract::TemporalKind::None;

  static constexpr std::array<std::string_view, 11> kDayParts{
      "noche", "tarde", "manana", "madrugada", "mediodia", "night",
      "afternoon", "morning", "dawn", "noon", "midday"};
  for (const auto& t : tokens) {
    for (const std::string_view d : kDayParts) {
      if (t == d) {
        out.kind = extract::TemporalKind::Relative;
        return;
      }
    }
  }

  out.kind = extract::TemporalKind::None;
}
