#include "number-words.hxx"

#include <array>
#include <string_view>

namespace
{
constexpr std::uint64_t kThousand = 1000;
constexpr std::uint64_t kMillion = 1000 * kThousand;
constexpr std::uint64_t kBillion = 1000 * kMillion;
constexpr std::uint64_t kTrillion = 1000 * kBillion;
constexpr std::uint64_t kLongScaleLimit = 1000 * kTrillion;

constexpr std::array<std::string_view, 30> kSpanishSmall{
    "cero",        "uno",          "dos",          "tres",        "cuatro",
    "cinco",       "seis",         "siete",        "ocho",        "nueve",
    "diez",        "once",         "doce",         "trece",       "catorce",
    "quince",      "dieciséis",    "diecisiete",   "dieciocho",   "diecinueve",
    "veinte",      "veintiuno",    "veintidós",    "veintitrés",  "veinticuatro",
    "veinticinco", "veintiséis",   "veintisiete",  "veintiocho",  "veintinueve"};

constexpr std::array<std::string_view, 10> kSpanishTens{
    "",          "diez",     "veinte",  "treinta", "cuarenta",
    "cincuenta", "sesenta",  "setenta", "ochenta", "noventa"};

constexpr std::array<std::string_view, 10> kSpanishHundreds{
    "",           "ciento",     "doscientos",  "trescientos", "cuatrocientos",
    "quinientos", "seiscientos", "setecientos", "ochocientos", "novecientos"};

constexpr std::array<std::string_view, 10> kSpanishFeminineHundreds{
    "",           "ciento",     "doscientas",  "trescientas", "cuatrocientas",
    "quinientas", "seiscientas", "setecientas", "ochocientas", "novecientas"};

constexpr std::array<std::string_view, 10> kSpanishOrdinalUnits{
    "",       "primero", "segundo", "tercero", "cuarto",
    "quinto", "sexto",   "séptimo", "octavo",  "noveno"};

constexpr std::array<std::string_view, 10> kSpanishOrdinalTens{
    "",              "décimo",       "vigésimo",     "trigésimo",   "cuadragésimo",
    "quincuagésimo", "sexagésimo",   "septuagésimo", "octogésimo",  "nonagésimo"};

constexpr std::array<std::string_view, 10> kSpanishOrdinalTeens{
    "décimo",      "undécimo",     "duodécimo",   "decimotercero", "decimocuarto",
    "decimoquinto", "decimosexto", "decimoséptimo", "decimoctavo", "decimonoveno"};

constexpr std::array<std::string_view, 20> kEnglishSmall{
    "zero",    "one",     "two",       "three",    "four",
    "five",    "six",     "seven",     "eight",    "nine",
    "ten",     "eleven",  "twelve",    "thirteen", "fourteen",
    "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};

constexpr std::array<std::string_view, 10> kEnglishTens{
    "",      "ten",   "twenty",  "thirty", "forty",
    "fifty", "sixty", "seventy", "eighty", "ninety"};

constexpr std::array<std::string_view, 12> kSpanishMonths{
    "enero", "febrero", "marzo",      "abril",   "mayo",      "junio",
    "julio", "agosto",  "septiembre", "octubre", "noviembre", "diciembre"};

constexpr std::array<std::string_view, 12> kEnglishMonths{
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};

std::string spanishUnitOne(SpanishGender gender)
{
  switch (gender) {
    case SpanishGender::Masculine:
      return "un";
    case SpanishGender::Feminine:
      return "una";
    case SpanishGender::Standalone:
      return "uno";
  }
  return "uno";
}

std::string spanishBelowHundred(const SpanishCardinalInput& input)
{
  const auto value = input.value;
  if (value == 1)
    return spanishUnitOne(input.gender);
  if (value == 21) {
    if (input.gender == SpanishGender::Masculine)
      return "veintiún";
    if (input.gender == SpanishGender::Feminine)
      return "veintiuna";
    return "veintiuno";
  }
  if (value < kSpanishSmall.size())
    return std::string(kSpanishSmall.at(value));
  std::string words(kSpanishTens.at(value / 10));
  const auto unit = value % 10;
  if (unit == 0)
    return words;
  words += " y ";
  words += unit == 1 ? spanishUnitOne(input.gender) : std::string(kSpanishSmall.at(unit));
  return words;
}

std::string spanishBelowThousand(const SpanishCardinalInput& input)
{
  const auto value = input.value;
  if (value == 100)
    return "cien";
  const auto hundreds = value / 100;
  const auto rest = value % 100;
  if (hundreds == 0)
    return spanishBelowHundred(input);
  const auto& table = input.gender == SpanishGender::Feminine ? kSpanishFeminineHundreds : kSpanishHundreds;
  std::string words(table.at(hundreds));
  if (rest != 0) {
    words += ' ';
    words += spanishBelowHundred({.value = rest, .gender = input.gender});
  }
  return words;
}

std::string spanishBelowMillion(const SpanishCardinalInput& input)
{
  const auto thousands = input.value / kThousand;
  const auto rest = input.value % kThousand;
  std::string words;
  if (thousands == 1)
    words = "mil";
  else if (thousands > 1) {
    const auto gender = input.gender == SpanishGender::Feminine ? SpanishGender::Feminine : SpanishGender::Masculine;
    words = spanishBelowThousand({.value = thousands, .gender = gender}) + " mil";
  }
  if (rest != 0) {
    if (!words.empty())
      words += ' ';
    words += spanishBelowThousand({.value = rest, .gender = input.gender});
  }
  return words;
}

struct ScaleWords
{
  std::uint64_t count{0};
  std::string_view singular;
  std::string_view plural;
};

std::string spanishScale(const ScaleWords& scale)
{
  if (scale.count == 1)
    return "un " + std::string(scale.singular);
  return spanishBelowMillion({.value = scale.count, .gender = SpanishGender::Masculine}) + " " +
         std::string(scale.plural);
}

std::string digitsByName(std::uint64_t value)
{
  const auto digits = std::to_string(value);
  std::string words;
  for (const char digit : digits) {
    if (!words.empty())
      words += ' ';
    words += kSpanishSmall.at(static_cast<std::size_t>(digit - '0'));
  }
  return words;
}

std::string feminize(std::string words)
{
  std::size_t position = 0;
  while (position < words.size()) {
    auto end = words.find(' ', position);
    if (end == std::string::npos)
      end = words.size();
    if (end > position && words[end - 1] == 'o')
      words[end - 1] = 'a';
    position = end + 1;
  }
  return words;
}

std::string englishBelowThousand(std::uint64_t value)
{
  std::string words;
  const auto hundreds = value / 100;
  const auto rest = value % 100;
  if (hundreds != 0) {
    words = std::string(kEnglishSmall.at(hundreds)) + " hundred";
    if (rest == 0)
      return words;
    words += ' ';
  }
  if (rest < kEnglishSmall.size())
    return words + std::string(kEnglishSmall.at(rest));
  words += kEnglishTens.at(rest / 10);
  if (rest % 10 != 0) {
    words += '-';
    words += kEnglishSmall.at(rest % 10);
  }
  return words;
}

std::string englishOrdinalWord(std::string_view word)
{
  if (word == "one")
    return "first";
  if (word == "two")
    return "second";
  if (word == "three")
    return "third";
  if (word == "five")
    return "fifth";
  if (word == "eight")
    return "eighth";
  if (word == "nine")
    return "ninth";
  if (word == "twelve")
    return "twelfth";
  if (word.ends_with('y'))
    return std::string(word.substr(0, word.size() - 1)) + "ieth";
  return std::string(word) + "th";
}
}

std::string spanishCardinal(const SpanishCardinalInput& input)
{
  const auto value = input.value;
  if (value == 0)
    return "cero";
  if (value >= kLongScaleLimit)
    return digitsByName(value);
  std::string words;
  const auto append = [&words](const std::string& part) {
    if (part.empty())
      return;
    if (!words.empty())
      words += ' ';
    words += part;
  };
  const auto trillions = value / kTrillion;
  const auto millions = (value / kMillion) % kMillion;
  const auto rest = value % kMillion;
  if (trillions != 0)
    append(spanishScale({.count = trillions, .singular = "billón", .plural = "billones"}));
  if (millions != 0)
    append(spanishScale({.count = millions, .singular = "millón", .plural = "millones"}));
  if (rest != 0)
    append(spanishBelowMillion({.value = rest, .gender = input.gender}));
  if ((trillions != 0 || millions != 0) && rest == 0 && input.gender != SpanishGender::Standalone)
    append("de");
  return words;
}

std::string spanishOrdinal(const SpanishOrdinalInput& input)
{
  const auto value = input.value;
  if (value == 0 || value >= 100)
    return spanishCardinal({.value = value, .gender = input.feminine ? SpanishGender::Feminine : SpanishGender::Masculine});
  std::string words;
  if (value < 10)
    words = std::string(kSpanishOrdinalUnits.at(value));
  else if (value < 20)
    words = std::string(kSpanishOrdinalTeens.at(value - 10));
  else {
    words = std::string(kSpanishOrdinalTens.at(value / 10));
    if (value % 10 != 0) {
      words += ' ';
      words += kSpanishOrdinalUnits.at(value % 10);
    }
  }
  if (input.feminine)
    return feminize(words);
  if (input.apocope && (words.ends_with("primero") || words.ends_with("tercero")))
    words.pop_back();
  return words;
}

std::string englishCardinal(std::uint64_t value)
{
  if (value == 0)
    return "zero";
  constexpr std::array<ScaleWords, 4> kScales{{{.count = kTrillion, .singular = "trillion", .plural = "trillion"},
                                               {.count = kBillion, .singular = "billion", .plural = "billion"},
                                               {.count = kMillion, .singular = "million", .plural = "million"},
                                               {.count = kThousand, .singular = "thousand", .plural = "thousand"}}};
  if (value >= kLongScaleLimit) {
    const auto digits = std::to_string(value);
    std::string words;
    for (const char digit : digits) {
      if (!words.empty())
        words += ' ';
      words += kEnglishSmall.at(static_cast<std::size_t>(digit - '0'));
    }
    return words;
  }
  std::string words;
  auto rest = value;
  for (const auto& scale : kScales) {
    const auto count = rest / scale.count;
    rest %= scale.count;
    if (count == 0)
      continue;
    if (!words.empty())
      words += ' ';
    words += englishBelowThousand(count) + " " + std::string(scale.singular);
  }
  if (rest != 0) {
    if (!words.empty())
      words += ' ';
    words += englishBelowThousand(rest);
  }
  return words;
}

std::string englishOrdinal(std::uint64_t value)
{
  auto words = englishCardinal(value);
  const auto split = words.find_last_of(" -");
  const auto head = split == std::string::npos ? std::string() : words.substr(0, split + 1);
  const auto last = split == std::string::npos ? std::string_view(words) : std::string_view(words).substr(split + 1);
  return head + englishOrdinalWord(last);
}

std::string englishYear(std::uint64_t value)
{
  if (value < 1000 || value > 9999 || (value >= 2000 && value < 2010))
    return englishCardinal(value);
  const auto high = value / 100;
  const auto low = value % 100;
  if (low == 0)
    return englishBelowThousand(high) + " hundred";
  if (low < 10)
    return englishBelowThousand(high) + " oh " + std::string(kEnglishSmall.at(low));
  return englishBelowThousand(high) + " " + englishBelowThousand(low);
}

std::string spanishMonth(int month)
{
  if (month < 1 || month > 12)
    return {};
  return std::string(kSpanishMonths.at(static_cast<std::size_t>(month - 1)));
}

std::string englishMonth(int month)
{
  if (month < 1 || month > 12)
    return {};
  return std::string(kEnglishMonths.at(static_cast<std::size_t>(month - 1)));
}
