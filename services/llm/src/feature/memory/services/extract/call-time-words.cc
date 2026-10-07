#include "call-time-words.hxx"

#include <algorithm>
#include <array>
#include <cctype>

namespace call_time
{

namespace
{
using Entry = std::pair<std::string_view, int>;

template <std::size_t N>
int valueOf(const std::array<Entry, N>& table, const std::string& token)
{
  const auto found = std::ranges::find_if(table, [&token](const Entry& entry) { return entry.first == token; });
  return found == table.end() ? -1 : found->second;
}

bool onlyDigits(const std::string& token)
{
  return !token.empty() && std::ranges::all_of(token, [](unsigned char c) { return std::isdigit(c) != 0; });
}

constexpr std::array<Entry, 32> kSmall{{{"un", 1},      {"una", 1},     {"uno", 1},       {"dos", 2},    {"tres", 3},    {"cuatro", 4},
                                         {"cinco", 5},  {"seis", 6},    {"siete", 7},     {"ocho", 8},   {"nueve", 9},   {"diez", 10},
                                         {"once", 11},  {"doce", 12},   {"quince", 15},   {"veinte", 20}, {"treinta", 30}, {"a", 1},
                                         {"an", 1},     {"one", 1},     {"two", 2},       {"three", 3},  {"four", 4},    {"five", 5},
                                         {"six", 6},    {"seven", 7},   {"eight", 8},     {"nine", 9},   {"ten", 10},    {"eleven", 11},
                                         {"twelve", 12}, {"fifteen", 15}}};

constexpr std::array<Entry, 31> kSpanishDays{{{"primero", 1},      {"uno", 1},         {"dos", 2},         {"tres", 3},
                                              {"cuatro", 4},       {"cinco", 5},       {"seis", 6},        {"siete", 7},
                                              {"ocho", 8},         {"nueve", 9},       {"diez", 10},       {"once", 11},
                                              {"doce", 12},        {"trece", 13},      {"catorce", 14},    {"quince", 15},
                                              {"dieciseis", 16},   {"diecisiete", 17}, {"dieciocho", 18},  {"diecinueve", 19},
                                              {"veinte", 20},      {"veintiuno", 21},  {"veintidos", 22},  {"veintitres", 23},
                                              {"veinticuatro", 24}, {"veinticinco", 25}, {"veintiseis", 26}, {"veintisiete", 27},
                                              {"veintiocho", 28},  {"veintinueve", 29}, {"treinta", 30}}};

constexpr std::array<Entry, 21> kEnglishOrdinals{{{"first", 1},       {"second", 2},      {"third", 3},       {"fourth", 4},
                                                  {"fifth", 5},       {"sixth", 6},       {"seventh", 7},     {"eighth", 8},
                                                  {"ninth", 9},       {"tenth", 10},      {"eleventh", 11},   {"twelfth", 12},
                                                  {"thirteenth", 13}, {"fourteenth", 14}, {"fifteenth", 15},  {"sixteenth", 16},
                                                  {"seventeenth", 17}, {"eighteenth", 18}, {"nineteenth", 19}, {"twentieth", 20},
                                                  {"thirtieth", 30}}};

constexpr std::array<Entry, 9> kEnglishUnits{{{"first", 1}, {"second", 2}, {"third", 3}, {"fourth", 4}, {"fifth", 5},
                                              {"sixth", 6}, {"seventh", 7}, {"eighth", 8}, {"ninth", 9}}};

constexpr std::array<Entry, 21> kEnglishCardinals{{{"one", 1},       {"two", 2},       {"three", 3},     {"four", 4},     {"five", 5},
                                                   {"six", 6},       {"seven", 7},     {"eight", 8},     {"nine", 9},     {"ten", 10},
                                                   {"eleven", 11},   {"twelve", 12},   {"thirteen", 13}, {"fourteen", 14}, {"fifteen", 15},
                                                   {"sixteen", 16},  {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19}, {"twenty", 20},
                                                   {"thirty", 30}}};

constexpr std::array<Entry, 6> kSpanishMinutes{{{"cuarto", 15}, {"media", 30}, {"cinco", 5}, {"diez", 10}, {"veinte", 20}, {"veinticinco", 25}}};

constexpr std::array<Entry, 15> kEnglishMinutes{{{"ten", 10},     {"eleven", 11},   {"twelve", 12},   {"thirteen", 13}, {"fourteen", 14},
                                                 {"fifteen", 15}, {"sixteen", 16},  {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19},
                                                 {"twenty", 20},  {"thirty", 30},   {"forty", 40},     {"fifty", 50},    {"five", 5}}};

constexpr std::array<Entry, 9> kEnglishDigits{{{"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6},
                                               {"seven", 7}, {"eight", 8}, {"nine", 9}}};

constexpr std::array<Entry, 36> kMonths{{{"enero", 1},      {"febrero", 2},   {"marzo", 3},      {"abril", 4},     {"mayo", 5},
                                         {"junio", 6},      {"julio", 7},     {"agosto", 8},     {"septiembre", 9}, {"setiembre", 9},
                                         {"octubre", 10},   {"noviembre", 11}, {"diciembre", 12}, {"january", 1},   {"february", 2},
                                         {"march", 3},      {"april", 4},     {"may", 5},        {"june", 6},      {"july", 7},
                                         {"august", 8},     {"september", 9}, {"october", 10},   {"november", 11}, {"december", 12},
                                         {"jan", 1},        {"feb", 2},       {"apr", 4},        {"jun", 6},       {"jul", 7},
                                         {"aug", 8},        {"sep", 9},       {"sept", 9},       {"oct", 10},      {"nov", 11},      {"dec", 12}}};

constexpr std::array<Entry, 14> kWeekdays{{{"domingo", 0},   {"lunes", 1},   {"martes", 2},   {"miercoles", 3}, {"jueves", 4},
                                           {"viernes", 5},   {"sabado", 6},  {"sunday", 0},   {"monday", 1},    {"tuesday", 2},
                                           {"wednesday", 3}, {"thursday", 4}, {"friday", 5},  {"saturday", 6}}};
}

Folded fold(std::string_view source)
{
  static constexpr std::array<std::pair<std::string_view, char>, 14> kFold{{{"á", 'a'},
                                                                           {"é", 'e'},
                                                                           {"í", 'i'},
                                                                           {"ó", 'o'},
                                                                           {"ú", 'u'},
                                                                           {"ü", 'u'},
                                                                           {"ñ", 'n'},
                                                                           {"Á", 'a'},
                                                                           {"É", 'e'},
                                                                           {"Í", 'i'},
                                                                           {"Ó", 'o'},
                                                                           {"Ú", 'u'},
                                                                           {"Ü", 'u'},
                                                                           {"Ñ", 'n'}}};
  Folded folded;
  folded.text.reserve(source.size());
  folded.origin.reserve(source.size() + 1);
  std::size_t index = 0;
  while (index < source.size()) {
    const auto match = std::ranges::find_if(kFold, [&](const auto& entry) { return source.substr(index, entry.first.size()) == entry.first; });
    if (match != kFold.end()) {
      folded.text.push_back(match->second);
      folded.origin.push_back(index);
      index += match->first.size();
      continue;
    }
    folded.text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(source[index]))));
    folded.origin.push_back(index);
    ++index;
  }
  folded.origin.push_back(source.size());
  return folded;
}

Tokens tokenize(const std::string& text)
{
  Tokens tokens;
  std::size_t index = 0;
  const auto digitAt = [&text](std::size_t at) { return at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0; };
  while (index < text.size()) {
    const auto first = static_cast<unsigned char>(text[index]);
    if (std::isalnum(first) == 0) {
      ++index;
      continue;
    }
    const std::size_t begin = index;
    const bool numeric = std::isdigit(first) != 0;
    while (index < text.size()) {
      const auto c = static_cast<unsigned char>(text[index]);
      if (numeric) {
        if (std::isdigit(c) != 0 || (c == ':' && index > begin && digitAt(index - 1) && digitAt(index + 1)))
          ++index;
        else
          break;
      }
      else if (std::isalnum(c) != 0)
        ++index;
      else
        break;
    }
    tokens.push_back({.text = text.substr(begin, index - begin), .begin = begin, .end = index});
  }
  return tokens;
}

bool wordAt(const Tokens& tokens, std::size_t at, std::string_view word)
{
  return at < tokens.size() && tokens[at].text == word;
}

bool anyWordAt(const Tokens& tokens, std::size_t at, std::initializer_list<std::string_view> words)
{
  return std::ranges::any_of(words, [&](std::string_view word) { return wordAt(tokens, at, word); });
}

int smallNumber(const std::string& token)
{
  if (const int value = valueOf(kSmall, token); value >= 0)
    return value;
  if (token.size() <= 3 && onlyDigits(token))
    return std::stoi(token);
  return -1;
}

std::optional<Counted> dayNumberAt(const Tokens& tokens, std::size_t at)
{
  if (at >= tokens.size())
    return std::nullopt;
  const std::string& head = tokens[at].text;
  if (onlyDigits(head)) {
    if (head.size() > 2)
      return std::nullopt;
    const int value = std::stoi(head);
    if (value < 1 || value > 31)
      return std::nullopt;
    const bool suffixed = anyWordAt(tokens, at + 1, {"st", "nd", "rd", "th"});
    return Counted{.value = value, .length = suffixed ? std::size_t{2} : std::size_t{1}, .ordinal = suffixed};
  }
  if (head == "treinta" && wordAt(tokens, at + 1, "y") && wordAt(tokens, at + 2, "uno"))
    return Counted{.value = 31, .length = 3, .ordinal = false};
  if (const int value = valueOf(kSpanishDays, head); value > 0)
    return Counted{.value = value, .length = 1, .ordinal = false};
  if (head == "twenty" || head == "thirty") {
    const int unit = at + 1 < tokens.size() ? valueOf(kEnglishUnits, tokens[at + 1].text) : -1;
    if (unit > 0 && (head == "twenty" || unit == 1))
      return Counted{.value = (head == "twenty" ? 20 : 30) + unit, .length = 2, .ordinal = true};
  }
  if (const int value = valueOf(kEnglishOrdinals, head); value > 0)
    return Counted{.value = value, .length = 1, .ordinal = true};
  return std::nullopt;
}

std::optional<Counted> cardinalDayAt(const Tokens& tokens, std::size_t at)
{
  if (at >= tokens.size())
    return std::nullopt;
  const std::string& head = tokens[at].text;
  const int value = valueOf(kEnglishCardinals, head);
  if (value <= 0)
    return std::nullopt;
  if ((head == "twenty" || head == "thirty") && at + 1 < tokens.size()) {
    const int unit = valueOf(kEnglishDigits, tokens[at + 1].text);
    if (unit > 0 && (head == "twenty" || unit == 1))
      return Counted{.value = value + unit, .length = 2, .ordinal = true};
  }
  return Counted{.value = value, .length = 1, .ordinal = true};
}

std::optional<Counted> minutesAt(const Tokens& tokens, std::size_t at)
{
  if (at >= tokens.size())
    return std::nullopt;
  const std::string& head = tokens[at].text;
  if (onlyDigits(head) && head.size() <= 2) {
    const int value = std::stoi(head);
    if (value >= 1 && value <= 59)
      return Counted{.value = value, .length = 1, .ordinal = false};
    return std::nullopt;
  }
  if (const int value = valueOf(kSpanishMinutes, head); value > 0)
    return Counted{.value = value, .length = 1, .ordinal = false};
  return std::nullopt;
}

std::optional<Counted> englishMinutesAt(const Tokens& tokens, std::size_t at)
{
  if (at >= tokens.size())
    return std::nullopt;
  const std::string& head = tokens[at].text;
  if (head == "oh" && at + 1 < tokens.size()) {
    const int unit = valueOf(kEnglishDigits, tokens[at + 1].text);
    if (unit > 0)
      return Counted{.value = unit, .length = 2, .ordinal = false};
    return std::nullopt;
  }
  const int value = valueOf(kEnglishMinutes, head);
  if (value <= 0)
    return std::nullopt;
  if (value >= 20 && at + 1 < tokens.size()) {
    const int unit = valueOf(kEnglishDigits, tokens[at + 1].text);
    if (unit > 0)
      return Counted{.value = value + unit, .length = 2, .ordinal = false};
  }
  return Counted{.value = value, .length = 1, .ordinal = false};
}

int monthOf(const std::string& token)
{
  const int value = valueOf(kMonths, token);
  return value > 0 ? value : 0;
}

int weekdayOf(const std::string& token)
{
  const auto found = std::ranges::find_if(kWeekdays, [&token](const Entry& entry) { return entry.first == token; });
  return found == kWeekdays.end() ? -1 : found->second;
}

}
