#include "text-normalizer.hxx"

#include "number-words.hxx"

#include <algorithm>
#include <array>
#include <charconv>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace
{
constexpr std::size_t kMaxCardinalDigits = 15;

struct Lexeme
{
  std::string_view written;
  std::string_view spoken;
  bool sentenceFinal{false};
};

struct UnitWords
{
  std::string_view written;
  std::string_view singular;
  std::string_view plural;
  bool feminine{false};
};

struct CurrencyWords
{
  std::string_view singular;
  std::string_view plural;
  std::string_view centSingular;
  std::string_view centPlural;
  bool feminine{false};
};

struct CurrencyMark
{
  std::string_view written;
  CurrencyWords spanish;
  CurrencyWords english;
};

constexpr std::array<Lexeme, 27> kSpanishAbbreviations{{
    {.written = "EE. UU.", .spoken = "Estados Unidos", .sentenceFinal = false},
    {.written = "EE.UU.", .spoken = "Estados Unidos", .sentenceFinal = false},
    {.written = "p. ej.", .spoken = "por ejemplo", .sentenceFinal = false},
    {.written = "Srta.", .spoken = "señorita", .sentenceFinal = false},
    {.written = "Sres.", .spoken = "señores", .sentenceFinal = false},
    {.written = "Sra.", .spoken = "señora", .sentenceFinal = false},
    {.written = "Sr.", .spoken = "señor", .sentenceFinal = false},
    {.written = "Dra.", .spoken = "doctora", .sentenceFinal = false},
    {.written = "Dr.", .spoken = "doctor", .sentenceFinal = false},
    {.written = "Uds.", .spoken = "ustedes", .sentenceFinal = false},
    {.written = "Ud.", .spoken = "usted", .sentenceFinal = false},
    {.written = "Lic.", .spoken = "licenciado", .sentenceFinal = false},
    {.written = "Ing.", .spoken = "ingeniero", .sentenceFinal = false},
    {.written = "Prof.", .spoken = "profesor", .sentenceFinal = false},
    {.written = "Avda.", .spoken = "avenida", .sentenceFinal = false},
    {.written = "Av.", .spoken = "avenida", .sentenceFinal = false},
    {.written = "aprox.", .spoken = "aproximadamente", .sentenceFinal = true},
    {.written = "pág.", .spoken = "página", .sentenceFinal = false},
    {.written = "Tel.", .spoken = "teléfono", .sentenceFinal = false},
    {.written = "tel.", .spoken = "teléfono", .sentenceFinal = false},
    {.written = "núm.", .spoken = "número", .sentenceFinal = false},
    {.written = "n.º", .spoken = "número", .sentenceFinal = false},
    {.written = "nº", .spoken = "número", .sentenceFinal = false},
    {.written = "dcha.", .spoken = "derecha", .sentenceFinal = false},
    {.written = "izqda.", .spoken = "izquierda", .sentenceFinal = false},
    {.written = "izq.", .spoken = "izquierda", .sentenceFinal = false},
    {.written = "etc.", .spoken = "etcétera", .sentenceFinal = true},
}};

constexpr std::array<Lexeme, 13> kEnglishAbbreviations{{
    {.written = "Mrs.", .spoken = "missus", .sentenceFinal = false},
    {.written = "Mr.", .spoken = "mister", .sentenceFinal = false},
    {.written = "Ms.", .spoken = "miz", .sentenceFinal = false},
    {.written = "Dr.", .spoken = "doctor", .sentenceFinal = false},
    {.written = "Prof.", .spoken = "professor", .sentenceFinal = false},
    {.written = "Jr.", .spoken = "junior", .sentenceFinal = false},
    {.written = "Sr.", .spoken = "senior", .sentenceFinal = false},
    {.written = "Ave.", .spoken = "avenue", .sentenceFinal = false},
    {.written = "approx.", .spoken = "approximately", .sentenceFinal = true},
    {.written = "vs.", .spoken = "versus", .sentenceFinal = false},
    {.written = "e.g.", .spoken = "for example", .sentenceFinal = false},
    {.written = "i.e.", .spoken = "that is", .sentenceFinal = false},
    {.written = "etc.", .spoken = "et cetera", .sentenceFinal = true},
}};

constexpr std::array<UnitWords, 29> kSpanishUnits{{
    {.written = "km/h", .singular = "kilómetro por hora", .plural = "kilómetros por hora", .feminine = false},
    {.written = "m/s", .singular = "metro por segundo", .plural = "metros por segundo", .feminine = false},
    {.written = "kWh", .singular = "kilovatio hora", .plural = "kilovatios hora", .feminine = false},
    {.written = "kW", .singular = "kilovatio", .plural = "kilovatios", .feminine = false},
    {.written = "km", .singular = "kilómetro", .plural = "kilómetros", .feminine = false},
    {.written = "cm", .singular = "centímetro", .plural = "centímetros", .feminine = false},
    {.written = "mm", .singular = "milímetro", .plural = "milímetros", .feminine = false},
    {.written = "kg", .singular = "kilo", .plural = "kilos", .feminine = false},
    {.written = "mg", .singular = "miligramo", .plural = "miligramos", .feminine = false},
    {.written = "ml", .singular = "mililitro", .plural = "mililitros", .feminine = false},
    {.written = "ms", .singular = "milisegundo", .plural = "milisegundos", .feminine = false},
    {.written = "min", .singular = "minuto", .plural = "minutos", .feminine = false},
    {.written = "seg", .singular = "segundo", .plural = "segundos", .feminine = false},
    {.written = "GB", .singular = "gigabyte", .plural = "gigabytes", .feminine = false},
    {.written = "MB", .singular = "megabyte", .plural = "megabytes", .feminine = false},
    {.written = "TB", .singular = "terabyte", .plural = "terabytes", .feminine = false},
    {.written = "Hz", .singular = "hercio", .plural = "hercios", .feminine = false},
    {.written = "°C", .singular = "grado", .plural = "grados", .feminine = false},
    {.written = "ºC", .singular = "grado", .plural = "grados", .feminine = false},
    {.written = "°F", .singular = "grado Fahrenheit", .plural = "grados Fahrenheit", .feminine = false},
    {.written = "°", .singular = "grado", .plural = "grados", .feminine = false},
    {.written = "m", .singular = "metro", .plural = "metros", .feminine = false},
    {.written = "g", .singular = "gramo", .plural = "gramos", .feminine = false},
    {.written = "l", .singular = "litro", .plural = "litros", .feminine = false},
    {.written = "L", .singular = "litro", .plural = "litros", .feminine = false},
    {.written = "h", .singular = "hora", .plural = "horas", .feminine = true},
    {.written = "s", .singular = "segundo", .plural = "segundos", .feminine = false},
    {.written = "W", .singular = "vatio", .plural = "vatios", .feminine = false},
    {.written = "V", .singular = "voltio", .plural = "voltios", .feminine = false},
}};

constexpr std::array<UnitWords, 31> kEnglishUnits{{
    {.written = "km/h", .singular = "kilometer per hour", .plural = "kilometers per hour", .feminine = false},
    {.written = "mph", .singular = "mile per hour", .plural = "miles per hour", .feminine = false},
    {.written = "m/s", .singular = "meter per second", .plural = "meters per second", .feminine = false},
    {.written = "kWh", .singular = "kilowatt hour", .plural = "kilowatt hours", .feminine = false},
    {.written = "kW", .singular = "kilowatt", .plural = "kilowatts", .feminine = false},
    {.written = "km", .singular = "kilometer", .plural = "kilometers", .feminine = false},
    {.written = "cm", .singular = "centimeter", .plural = "centimeters", .feminine = false},
    {.written = "mm", .singular = "millimeter", .plural = "millimeters", .feminine = false},
    {.written = "kg", .singular = "kilogram", .plural = "kilograms", .feminine = false},
    {.written = "mg", .singular = "milligram", .plural = "milligrams", .feminine = false},
    {.written = "ml", .singular = "milliliter", .plural = "milliliters", .feminine = false},
    {.written = "ms", .singular = "millisecond", .plural = "milliseconds", .feminine = false},
    {.written = "min", .singular = "minute", .plural = "minutes", .feminine = false},
    {.written = "sec", .singular = "second", .plural = "seconds", .feminine = false},
    {.written = "mi", .singular = "mile", .plural = "miles", .feminine = false},
    {.written = "ft", .singular = "foot", .plural = "feet", .feminine = false},
    {.written = "lbs", .singular = "pound", .plural = "pounds", .feminine = false},
    {.written = "lb", .singular = "pound", .plural = "pounds", .feminine = false},
    {.written = "GB", .singular = "gigabyte", .plural = "gigabytes", .feminine = false},
    {.written = "MB", .singular = "megabyte", .plural = "megabytes", .feminine = false},
    {.written = "TB", .singular = "terabyte", .plural = "terabytes", .feminine = false},
    {.written = "Hz", .singular = "hertz", .plural = "hertz", .feminine = false},
    {.written = "°C", .singular = "degree Celsius", .plural = "degrees Celsius", .feminine = false},
    {.written = "°F", .singular = "degree Fahrenheit", .plural = "degrees Fahrenheit", .feminine = false},
    {.written = "°", .singular = "degree", .plural = "degrees", .feminine = false},
    {.written = "m", .singular = "meter", .plural = "meters", .feminine = false},
    {.written = "g", .singular = "gram", .plural = "grams", .feminine = false},
    {.written = "h", .singular = "hour", .plural = "hours", .feminine = false},
    {.written = "s", .singular = "second", .plural = "seconds", .feminine = false},
    {.written = "W", .singular = "watt", .plural = "watts", .feminine = false},
    {.written = "V", .singular = "volt", .plural = "volts", .feminine = false},
}};

constexpr CurrencyWords kSpanishEuro{
    .singular = "euro", .plural = "euros", .centSingular = "céntimo", .centPlural = "céntimos", .feminine = false};
constexpr CurrencyWords kSpanishDollar{
    .singular = "dólar", .plural = "dólares", .centSingular = "centavo", .centPlural = "centavos", .feminine = false};
constexpr CurrencyWords kSpanishPeso{.singular = "peso mexicano",
                                     .plural = "pesos mexicanos",
                                     .centSingular = "centavo",
                                     .centPlural = "centavos",
                                     .feminine = false};
constexpr CurrencyWords kSpanishPound{
    .singular = "libra", .plural = "libras", .centSingular = "penique", .centPlural = "peniques", .feminine = true};
constexpr CurrencyWords kEnglishEuro{
    .singular = "euro", .plural = "euros", .centSingular = "cent", .centPlural = "cents", .feminine = false};
constexpr CurrencyWords kEnglishDollar{
    .singular = "dollar", .plural = "dollars", .centSingular = "cent", .centPlural = "cents", .feminine = false};
constexpr CurrencyWords kEnglishPeso{
    .singular = "peso", .plural = "pesos", .centSingular = "centavo", .centPlural = "centavos", .feminine = false};
constexpr CurrencyWords kEnglishPound{
    .singular = "pound", .plural = "pounds", .centSingular = "penny", .centPlural = "pence", .feminine = false};

constexpr std::array<CurrencyMark, 8> kCurrencies{{
    {.written = "US$", .spanish = kSpanishDollar, .english = kEnglishDollar},
    {.written = "€", .spanish = kSpanishEuro, .english = kEnglishEuro},
    {.written = "$", .spanish = kSpanishDollar, .english = kEnglishDollar},
    {.written = "£", .spanish = kSpanishPound, .english = kEnglishPound},
    {.written = "EUR", .spanish = kSpanishEuro, .english = kEnglishEuro},
    {.written = "USD", .spanish = kSpanishDollar, .english = kEnglishDollar},
    {.written = "MXN", .spanish = kSpanishPeso, .english = kEnglishPeso},
    {.written = "GBP", .spanish = kSpanishPound, .english = kEnglishPound},
}};

constexpr std::array<std::string_view, 21> kSpanishMasculineInA{
    "día",    "días",    "mediodía", "problema", "problemas", "sistema", "sistemas",
    "mapa",   "mapas",   "programa", "programas", "tema",     "temas",   "idioma",
    "idiomas", "planeta", "planetas", "clima",    "esquema",  "drama",   "poema"};

constexpr std::array<std::string_view, 21> kSpanishFunctionWords{
    "de",   "del", "y",   "e",     "o",     "u",     "a",    "al",   "en",  "por", "para",
    "con",  "sin", "que", "más",   "menos", "entre", "sobre", "hasta", "desde", "coma"};

constexpr std::string_view kNoBreakSpace = "\xC2\xA0";
constexpr std::string_view kNarrowNoBreakSpace = "\xE2\x80\xAF";

struct Codepoint
{
  char32_t value{0};
  std::size_t length{0};
};

Codepoint decodeAt(std::string_view text, std::size_t position)
{
  if (position >= text.size())
    return {.value = 0, .length = 0};
  const auto lead = static_cast<unsigned char>(text[position]);
  const auto continuation = [&text, position](std::size_t offset) -> char32_t {
    if (position + offset >= text.size())
      return 0;
    return static_cast<char32_t>(static_cast<unsigned char>(text[position + offset]) & 0x3FU);
  };
  const auto value = static_cast<char32_t>(lead);
  if (lead < 0x80U)
    return {.value = value, .length = 1};
  if ((lead & 0xE0U) == 0xC0U)
    return {.value = static_cast<char32_t>(((value & 0x1FU) << 6U) | continuation(1)), .length = 2};
  if ((lead & 0xF0U) == 0xE0U)
    return {.value = static_cast<char32_t>(((value & 0x0FU) << 12U) | (continuation(1) << 6U) | continuation(2)),
            .length = 3};
  return {.value = static_cast<char32_t>(((value & 0x07U) << 18U) | (continuation(1) << 12U) |
                                         (continuation(2) << 6U) | continuation(3)),
          .length = 4};
}

Codepoint decodeBefore(std::string_view text, std::size_t position)
{
  if (position == 0)
    return {.value = 0, .length = 0};
  auto start = position - 1;
  while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0U) == 0x80U)
    --start;
  const auto decoded = decodeAt(text, start);
  return {.value = decoded.value, .length = position - start};
}

bool isLetter(char32_t value)
{
  return (value >= U'a' && value <= U'z') || (value >= U'A' && value <= U'Z') ||
         (value >= 0xC0U && value <= 0x24FU && value != 0xD7U && value != 0xF7U);
}

bool isDigit(char32_t value)
{
  return value >= U'0' && value <= U'9';
}

bool isUppercase(char32_t value)
{
  return (value >= U'A' && value <= U'Z') || (value >= 0xC0U && value <= 0xDEU && value != 0xD7U);
}

bool isDigitChar(char value)
{
  return value >= '0' && value <= '9';
}

bool isSpaceChar(char value)
{
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

std::string lowerAscii(std::string_view text)
{
  std::string lowered(text);
  std::ranges::transform(lowered, lowered.begin(), [](char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
  });
  return lowered;
}

std::optional<std::uint64_t> parseUnsigned(std::string_view digits)
{
  std::uint64_t value = 0;
  const auto* const begin = std::to_address(digits.begin());
  const auto* const end = std::to_address(digits.end());
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end)
    return std::nullopt;
  return value;
}

struct ParsedNumber
{
  std::string integerDigits;
  std::string fractionDigits;
  char decimalMark{'.'};
  bool negative{false};
  std::size_t length{0};
};

class Normalizer
{
public:
  Normalizer(std::string_view text, SpeechLanguage language) : source_(text), language_(language) {}

  std::string run()
  {
    expandAbbreviations();
    verbalizeNumbers();
    replaceSymbols();
    return collapseWhitespace(text_);
  }

private:
  [[nodiscard]] bool spanish() const { return language_ == SpeechLanguage::Spanish; }

  void expandAbbreviations()
  {
    const auto table = spanish() ? std::span<const Lexeme>(kSpanishAbbreviations) : std::span<const Lexeme>(kEnglishAbbreviations);
    std::string out;
    out.reserve(source_.size() + 16);
    std::size_t position = 0;
    while (position < source_.size()) {
      if (const auto consumed = expandAt({.table = table, .position = position, .out = out}); consumed > 0) {
        position += consumed;
        continue;
      }
      out += source_[position];
      ++position;
    }
    text_ = std::move(out);
  }

  struct ExpandInput
  {
    std::span<const Lexeme> table;
    std::size_t position{0};
    std::string& out;
  };

  std::size_t expandAt(const ExpandInput& input)
  {
    const auto position = input.position;
    const auto before = decodeBefore(source_, position);
    if (isLetter(before.value) || isDigit(before.value))
      return 0;
    if (source_[position] == '#' && position + 1 < source_.size() && isDigitChar(source_[position + 1])) {
      input.out += spanish() ? "número " : "number ";
      return 1;
    }
    if (!spanish() && source_.substr(position).starts_with("No.")) {
      auto next = position + 3;
      while (next < source_.size() && source_[next] == ' ')
        ++next;
      if (next < source_.size() && isDigitChar(source_[next])) {
        input.out += "number";
        return 3;
      }
      return 0;
    }
    for (const auto& lexeme : input.table) {
      if (!source_.substr(position).starts_with(lexeme.written))
        continue;
      const auto end = position + lexeme.written.size();
      if (!lexeme.written.ends_with('.')) {
        const auto after = decodeAt(source_, end);
        if (isLetter(after.value) || isDigit(after.value))
          continue;
      }
      input.out += lexeme.spoken;
      if (lexeme.written.ends_with('.') && sentenceEndsAfter({.position = end, .sentenceFinal = lexeme.sentenceFinal}))
        input.out += '.';
      return lexeme.written.size();
    }
    return 0;
  }

  struct SentenceEndInput
  {
    std::size_t position{0};
    bool sentenceFinal{false};
  };

  [[nodiscard]] bool sentenceEndsAfter(const SentenceEndInput& input) const
  {
    auto next = input.position;
    while (next < source_.size() && (source_[next] == ' ' || source_[next] == '\t'))
      ++next;
    if (next >= source_.size() || source_[next] == '\n')
      return true;
    if (!input.sentenceFinal || next == input.position)
      return false;
    return isUppercase(decodeAt(source_, next).value);
  }

  void verbalizeNumbers()
  {
    const std::string text = std::move(text_);
    std::string out;
    out.reserve(text.size() * 2);
    std::size_t position = 0;
    while (position < text.size()) {
      const char current = text[position];
      const bool startsNumber =
          isDigitChar(current) || (current == '-' && position + 1 < text.size() && isDigitChar(text[position + 1]) &&
                                   (position == 0 || isSpaceChar(text[position - 1]) || text[position - 1] == '('));
      if (startsNumber && !isLetter(decodeBefore(text, position).value)) {
        if (const auto consumed = verbalizeAt({.text = text, .position = position, .out = out}); consumed > 0) {
          position += consumed;
          continue;
        }
      }
      out += current;
      ++position;
    }
    text_ = std::move(out);
  }

  struct VerbalizeInput
  {
    std::string_view text;
    std::size_t position{0};
    std::string& out;
  };

  std::size_t verbalizeAt(const VerbalizeInput& input)
  {
    if (isDigitChar(input.text[input.position])) {
      if (const auto consumed = verbalizeTime(input); consumed > 0)
        return consumed;
      if (const auto consumed = verbalizeDate(input); consumed > 0)
        return consumed;
      if (const auto consumed = verbalizeOrdinal(input); consumed > 0)
        return consumed;
    }
    return verbalizeQuantity(input);
  }

  struct DigitRun
  {
    std::string_view digits;
    std::size_t end{0};
  };

  static DigitRun digitRun(std::string_view text, std::size_t position)
  {
    auto end = position;
    while (end < text.size() && isDigitChar(text[end]))
      ++end;
    return {.digits = text.substr(position, end - position), .end = end};
  }

  static bool boundaryAt(std::string_view text, std::size_t position)
  {
    const auto after = decodeAt(text, position);
    return !isLetter(after.value) && !isDigit(after.value);
  }

  std::size_t verbalizeTime(const VerbalizeInput& input)
  {
    const auto& text = input.text;
    const auto hours = digitRun(text, input.position);
    if (hours.digits.empty() || hours.digits.size() > 2 || hours.end >= text.size() || text[hours.end] != ':')
      return 0;
    const auto minutes = digitRun(text, hours.end + 1);
    if (minutes.digits.size() != 2 || !boundaryAt(text, minutes.end) ||
        (minutes.end < text.size() && text[minutes.end] == ':'))
      return 0;
    const auto hour = parseUnsigned(hours.digits).value_or(99);
    const auto minute = parseUnsigned(minutes.digits).value_or(99);
    if (hour > 23 || minute > 59)
      return 0;
    auto end = minutes.end;
    std::optional<bool> afternoon;
    if (!spanish())
      end = consumeMeridiem({.text = text, .position = end, .afternoon = afternoon});
    else
      end = consumeHourSuffix(text, end);
    auto hour24 = hour;
    if (afternoon.has_value()) {
      hour24 = hour % 12;
      if (*afternoon)
        hour24 += 12;
    }
    const bool explicitPeriod = afternoon.has_value() || hour24 >= 13 || hour24 == 0 || hours.digits.starts_with('0');
    const ClockReading reading{.hour = hour24, .minute = minute, .withPeriod = explicitPeriod};
    input.out += spanish() ? spanishTime({.reading = reading, .out = input.out}) : englishTime(reading);
    return end - input.position;
  }

  struct ClockReading
  {
    std::uint64_t hour{0};
    std::uint64_t minute{0};
    bool withPeriod{false};
  };

  struct MeridiemInput
  {
    std::string_view text;
    std::size_t position{0};
    std::optional<bool>& afternoon;
  };

  static std::size_t consumeMeridiem(const MeridiemInput& input)
  {
    auto next = input.position;
    if (next < input.text.size() && input.text[next] == ' ')
      ++next;
    const auto rest = lowerAscii(input.text.substr(next, 4));
    for (const auto& [written, isAfternoon] : std::array<std::pair<std::string_view, bool>, 4>{
             {{"a.m.", false}, {"p.m.", true}, {"am", false}, {"pm", true}}}) {
      if (rest.starts_with(written) && boundaryAt(input.text, next + written.size())) {
        input.afternoon = isAfternoon;
        return next + written.size();
      }
    }
    return input.position;
  }

  static std::size_t consumeHourSuffix(std::string_view text, std::size_t position)
  {
    auto next = position;
    if (next < text.size() && text[next] == ' ')
      ++next;
    for (const std::string_view suffix : {std::string_view("horas"), std::string_view("hrs"), std::string_view("h")}) {
      if (text.substr(next).starts_with(suffix) && boundaryAt(text, next + suffix.size()))
        return next + suffix.size();
    }
    return position;
  }

  struct SpanishTimeInput
  {
    ClockReading reading;
    const std::string& out;
  };

  static std::string spanishTime(const SpanishTimeInput& input)
  {
    const auto& reading = input.reading;
    const auto twelve = reading.hour % 12 == 0 ? 12 : reading.hour % 12;
    const auto tail = lowerAscii(std::string_view(input.out).substr(input.out.size() > 4 ? input.out.size() - 4 : 0));
    const bool hasArticle = tail.ends_with("las ") || tail.ends_with(" la ") || tail == "la ";
    std::string words;
    if (!hasArticle)
      words = twelve == 1 ? "la " : "las ";
    words += spanishCardinal({.value = twelve, .gender = SpanishGender::Feminine});
    if (reading.minute == 15)
      words += " y cuarto";
    else if (reading.minute == 30)
      words += " y media";
    else if (reading.minute != 0)
      words += " y " + spanishCardinal({.value = reading.minute, .gender = SpanishGender::Standalone});
    if (!reading.withPeriod)
      return words;
    if (reading.hour == 0 || reading.hour >= 20)
      words += " de la noche";
    else if (reading.hour < 6)
      words += " de la madrugada";
    else if (reading.hour < 12)
      words += " de la mañana";
    else if (reading.hour < 13)
      words += " del mediodía";
    else
      words += " de la tarde";
    return words;
  }

  static std::string englishTime(const ClockReading& reading)
  {
    const auto twelve = reading.hour % 12 == 0 ? 12 : reading.hour % 12;
    std::string words = englishCardinal(twelve);
    if (reading.minute == 0)
      words += " o'clock";
    else if (reading.minute < 10)
      words += " oh " + englishCardinal(reading.minute);
    else
      words += " " + englishCardinal(reading.minute);
    if (!reading.withPeriod)
      return words;
    if (reading.hour < 5 || reading.hour >= 22)
      words += " at night";
    else if (reading.hour < 12)
      words += " in the morning";
    else if (reading.hour < 18)
      words += " in the afternoon";
    else
      words += " in the evening";
    return words;
  }

  struct CalendarDate
  {
    std::uint64_t year{0};
    int month{0};
    std::uint64_t day{0};
  };

  std::size_t verbalizeDate(const VerbalizeInput& input)
  {
    const auto& text = input.text;
    const auto first = digitRun(text, input.position);
    if (first.end >= text.size() || (text[first.end] != '/' && text[first.end] != '-'))
      return 0;
    const char separator = text[first.end];
    const auto second = digitRun(text, first.end + 1);
    if (second.digits.empty() || second.end >= text.size() || text[second.end] != separator)
      return 0;
    const auto third = digitRun(text, second.end + 1);
    if (third.digits.empty() || !boundaryAt(text, third.end))
      return 0;
    const auto a = parseUnsigned(first.digits).value_or(0);
    const auto b = parseUnsigned(second.digits).value_or(0);
    const auto c = parseUnsigned(third.digits).value_or(0);
    CalendarDate date;
    if (separator == '-' && first.digits.size() == 4 && second.digits.size() == 2 && third.digits.size() == 2)
      date = {.year = a, .month = static_cast<int>(b), .day = c};
    else if (separator == '/' && first.digits.size() <= 2 && second.digits.size() <= 2 && third.digits.size() == 4)
      date = spanish() ? CalendarDate{.year = c, .month = static_cast<int>(b), .day = a}
                       : CalendarDate{.year = c, .month = static_cast<int>(a), .day = b};
    else
      return 0;
    if (date.month < 1 || date.month > 12 || date.day < 1 || date.day > 31)
      return 0;
    if (spanish())
      input.out += spanishCardinal({.value = date.day, .gender = SpanishGender::Standalone}) + " de " +
                   spanishMonth(date.month) + " de " +
                   spanishCardinal({.value = date.year, .gender = SpanishGender::Standalone});
    else
      input.out += englishMonth(date.month) + " " + englishOrdinal(date.day) + ", " + englishYear(date.year);
    return third.end - input.position;
  }

  std::size_t verbalizeOrdinal(const VerbalizeInput& input)
  {
    const auto& text = input.text;
    const auto run = digitRun(text, input.position);
    if (run.digits.size() > 3)
      return 0;
    const auto value = parseUnsigned(run.digits).value_or(0);
    const auto rest = text.substr(run.end);
    if (spanish()) {
      struct Suffix
      {
        std::string_view written;
        bool feminine{false};
        bool apocope{false};
      };
      constexpr std::array<Suffix, 8> kSuffixes{{{.written = ".º", .feminine = false, .apocope = false},
                                                 {.written = "º", .feminine = false, .apocope = false},
                                                 {.written = ".ª", .feminine = true, .apocope = false},
                                                 {.written = "ª", .feminine = true, .apocope = false},
                                                 {.written = ".era", .feminine = true, .apocope = false},
                                                 {.written = "era", .feminine = true, .apocope = false},
                                                 {.written = ".er", .feminine = false, .apocope = true},
                                                 {.written = "er", .feminine = false, .apocope = true}}};
      for (const auto& suffix : kSuffixes) {
        if (!rest.starts_with(suffix.written) || !boundaryAt(text, run.end + suffix.written.size()))
          continue;
        input.out += spanishOrdinal({.value = value, .feminine = suffix.feminine, .apocope = suffix.apocope});
        return run.digits.size() + suffix.written.size();
      }
      return 0;
    }
    const auto suffix = lowerAscii(rest.substr(0, 2));
    if ((suffix == "st" || suffix == "nd" || suffix == "rd" || suffix == "th") && boundaryAt(text, run.end + 2)) {
      input.out += englishOrdinal(value);
      return run.digits.size() + 2;
    }
    return 0;
  }

  [[nodiscard]] ParsedNumber parseNumber(std::string_view text, std::size_t position) const
  {
    ParsedNumber number;
    auto cursor = position;
    if (text[cursor] == '-') {
      number.negative = true;
      ++cursor;
    }
    const auto leading = digitRun(text, cursor);
    const char groupMark = spanish() ? '.' : ',';
    auto groupedEnd = leading.end;
    std::string grouped(leading.digits);
    std::size_t groups = 0;
    if (leading.digits.size() <= 3) {
      while (true) {
        std::size_t separatorLength = 0;
        if (groupedEnd < text.size() && text[groupedEnd] == groupMark)
          separatorLength = 1;
        else if (spanish() && text.substr(groupedEnd).starts_with(kNoBreakSpace))
          separatorLength = kNoBreakSpace.size();
        else if (spanish() && text.substr(groupedEnd).starts_with(kNarrowNoBreakSpace))
          separatorLength = kNarrowNoBreakSpace.size();
        if (separatorLength == 0)
          break;
        const auto group = digitRun(text, groupedEnd + separatorLength);
        if (group.digits.size() != 3)
          break;
        grouped += group.digits;
        groupedEnd = group.end;
        ++groups;
      }
    }
    if (groups > 0) {
      number.integerDigits = grouped;
      cursor = groupedEnd;
      const char decimalMark = spanish() ? ',' : '.';
      if (cursor + 1 < text.size() && text[cursor] == decimalMark && isDigitChar(text[cursor + 1])) {
        const auto fraction = digitRun(text, cursor + 1);
        number.fractionDigits = fraction.digits;
        number.decimalMark = decimalMark;
        cursor = fraction.end;
      }
    }
    else {
      number.integerDigits = leading.digits;
      cursor = leading.end;
      const bool markAllowed = cursor + 1 < text.size() && (text[cursor] == '.' || (spanish() && text[cursor] == ','));
      if (markAllowed && isDigitChar(text[cursor + 1])) {
        const auto fraction = digitRun(text, cursor + 1);
        number.fractionDigits = fraction.digits;
        number.decimalMark = text[cursor];
        cursor = fraction.end;
      }
    }
    number.length = cursor - position;
    return number;
  }

  [[nodiscard]] std::string digitsByName(std::string_view digits) const
  {
    std::string words;
    for (const char digit : digits) {
      if (!words.empty())
        words += ' ';
      const auto value = static_cast<std::uint64_t>(digit - '0');
      words += spanish() ? spanishCardinal({.value = value, .gender = SpanishGender::Standalone}) : englishCardinal(value);
    }
    return words;
  }

  [[nodiscard]] std::string integerWords(std::string_view digits, SpanishGender gender) const
  {
    const auto value = parseUnsigned(digits);
    if (!value || digits.size() > kMaxCardinalDigits || (digits.size() > 1 && digits.starts_with('0')))
      return digitsByName(digits);
    return spanish() ? spanishCardinal({.value = *value, .gender = gender}) : englishCardinal(*value);
  }

  struct NumberWordsInput
  {
    const ParsedNumber& number;
    SpanishGender gender{SpanishGender::Standalone};
  };

  [[nodiscard]] std::string numberWords(const NumberWordsInput& input) const
  {
    const auto& number = input.number;
    std::string words;
    if (number.negative)
      words = spanish() ? "menos " : "minus ";
    if (number.fractionDigits.empty())
      return words + integerWords(number.integerDigits, input.gender);
    words += integerWords(number.integerDigits, SpanishGender::Standalone);
    if (!spanish())
      return words + " point " + digitsByName(number.fractionDigits);
    words += number.decimalMark == ',' ? " coma " : " punto ";
    if (number.fractionDigits.size() <= 2 && !number.fractionDigits.starts_with('0'))
      return words + integerWords(number.fractionDigits, SpanishGender::Standalone);
    return words + digitsByName(number.fractionDigits);
  }

  [[nodiscard]] static bool isOne(const ParsedNumber& number)
  {
    return number.fractionDigits.empty() && !number.negative && number.integerDigits == "1";
  }

  struct Lookahead
  {
    std::size_t position{0};
    std::size_t consumed{0};
  };

  static Lookahead skipOneSpace(std::string_view text, std::size_t position)
  {
    if (position < text.size() && text[position] == ' ')
      return {.position = position + 1, .consumed = 1};
    if (text.substr(position).starts_with(kNoBreakSpace))
      return {.position = position + kNoBreakSpace.size(), .consumed = kNoBreakSpace.size()};
    if (text.substr(position).starts_with(kNarrowNoBreakSpace))
      return {.position = position + kNarrowNoBreakSpace.size(), .consumed = kNarrowNoBreakSpace.size()};
    return {.position = position, .consumed = 0};
  }

  [[nodiscard]] const CurrencyMark* currencyAt(std::string_view text, std::size_t position) const
  {
    for (const auto& mark : kCurrencies) {
      if (!text.substr(position).starts_with(mark.written))
        continue;
      const bool symbol = !isLetter(decodeAt(mark.written, 0).value);
      if (symbol || boundaryAt(text, position + mark.written.size()))
        return &mark;
    }
    return nullptr;
  }

  const CurrencyMark* takePrefixCurrency(std::string& out) const
  {
    auto end = out.size();
    while (end > 0 && out[end - 1] == ' ')
      --end;
    if (end + 1 < out.size())
      return nullptr;
    for (const auto& mark : kCurrencies) {
      if (end < mark.written.size() || std::string_view(out).substr(end - mark.written.size(), mark.written.size()) != mark.written)
        continue;
      const auto start = end - mark.written.size();
      if (isLetter(decodeBefore(out, start).value))
        continue;
      out.erase(start);
      if (!out.empty() && out.back() != ' ' && out.back() != '\n')
        out += ' ';
      return &mark;
    }
    return nullptr;
  }

  [[nodiscard]] const UnitWords* unitAt(std::string_view text, std::size_t position) const
  {
    const auto units = spanish() ? std::span<const UnitWords>(kSpanishUnits) : std::span<const UnitWords>(kEnglishUnits);
    for (const auto& unit : units) {
      if (text.substr(position).starts_with(unit.written) && boundaryAt(text, position + unit.written.size()))
        return &unit;
    }
    return nullptr;
  }

  [[nodiscard]] SpanishGender genderOfNextWord(std::string_view text, std::size_t position) const
  {
    if (!spanish())
      return SpanishGender::Standalone;
    auto start = position;
    while (start < text.size() && text[start] == ' ')
      ++start;
    if (start == position)
      return SpanishGender::Standalone;
    auto end = start;
    while (end < text.size()) {
      const auto codepoint = decodeAt(text, end);
      if (!isLetter(codepoint.value))
        break;
      end += codepoint.length;
    }
    if (end == start)
      return SpanishGender::Standalone;
    const auto word = lowerAscii(text.substr(start, end - start));
    if (std::ranges::find(kSpanishFunctionWords, word) != kSpanishFunctionWords.end())
      return SpanishGender::Standalone;
    if (std::ranges::find(kSpanishMasculineInA, word) != kSpanishMasculineInA.end())
      return SpanishGender::Masculine;
    if (word.ends_with('a') || word.ends_with("as"))
      return SpanishGender::Feminine;
    return SpanishGender::Masculine;
  }

  std::size_t verbalizeQuantity(const VerbalizeInput& input)
  {
    const auto& text = input.text;
    const auto number = parseNumber(text, input.position);
    if (number.integerDigits.empty())
      return 0;
    auto end = input.position + number.length;
    const auto* currency = takePrefixCurrency(input.out);
    const auto gap = skipOneSpace(text, end);
    if (currency == nullptr) {
      if (const auto* suffix = currencyAt(text, gap.position); suffix != nullptr) {
        currency = suffix;
        end = gap.position + suffix->written.size();
      }
    }
    else if (const auto* suffix = currencyAt(text, gap.position); suffix != nullptr && suffix->written == currency->written)
      end = gap.position + suffix->written.size();
    if (currency != nullptr) {
      input.out += currencyWords({.number = number, .currency = *currency});
      return end - input.position;
    }
    if (gap.position < text.size() && text[gap.position] == '%') {
      input.out += numberWords({.number = number, .gender = SpanishGender::Standalone});
      input.out += spanish() ? " por ciento" : " percent";
      return gap.position + 1 - input.position;
    }
    if (const auto* unit = unitAt(text, gap.position); unit != nullptr) {
      const auto gender = unit->feminine ? SpanishGender::Feminine : SpanishGender::Masculine;
      input.out += numberWords({.number = number, .gender = gender});
      input.out += ' ';
      input.out += isOne(number) ? unit->singular : unit->plural;
      return gap.position + unit->written.size() - input.position;
    }
    input.out += numberWords({.number = number, .gender = genderOfNextWord(text, end)});
    if (!boundaryAt(text, end))
      input.out += ' ';
    return end - input.position;
  }

  struct CurrencyInput
  {
    const ParsedNumber& number;
    const CurrencyMark& currency;
  };

  [[nodiscard]] std::string currencyWords(const CurrencyInput& input) const
  {
    const auto& number = input.number;
    const auto& words = spanish() ? input.currency.spanish : input.currency.english;
    const auto whole = parseUnsigned(number.integerDigits);
    if (!whole || number.integerDigits.size() > kMaxCardinalDigits)
      return numberWords({.number = number, .gender = SpanishGender::Standalone}) + " " + std::string(words.plural);
    std::uint64_t cents = 0;
    if (!number.fractionDigits.empty()) {
      auto centDigits = number.fractionDigits.substr(0, 2);
      if (centDigits.size() == 1)
        centDigits += '0';
      cents = parseUnsigned(centDigits).value_or(0);
    }
    const auto gender = words.feminine ? SpanishGender::Feminine : SpanishGender::Masculine;
    const auto amount = [this, gender](std::uint64_t value) {
      return spanish() ? spanishCardinal({.value = value, .gender = gender}) : englishCardinal(value);
    };
    std::string spoken;
    if (number.negative)
      spoken = spanish() ? "menos " : "minus ";
    if (*whole > 0 || cents == 0)
      spoken += amount(*whole) + " " + std::string(*whole == 1 ? words.singular : words.plural);
    if (cents > 0) {
      if (*whole > 0)
        spoken += spanish() ? " con " : " and ";
      const auto centGender = SpanishGender::Masculine;
      spoken += spanish() ? spanishCardinal({.value = cents, .gender = centGender}) : englishCardinal(cents);
      spoken += ' ';
      spoken += cents == 1 ? words.centSingular : words.centPlural;
    }
    return spoken;
  }

  void replaceSymbols()
  {
    struct Symbol
    {
      std::string_view written;
      std::string_view spanish;
      std::string_view english;
    };
    constexpr std::array<Symbol, 10> kSymbols{{
        {.written = "&", .spanish = " y ", .english = " and "},
        {.written = "@", .spanish = " arroba ", .english = " at "},
        {.written = "=", .spanish = " igual a ", .english = " equals "},
        {.written = "+", .spanish = " más ", .english = " plus "},
        {.written = "×", .spanish = " por ", .english = " times "},
        {.written = "%", .spanish = " por ciento", .english = " percent"},
        {.written = "°", .spanish = " grados", .english = " degrees"},
        {.written = "*", .spanish = "", .english = ""},
        {.written = "`", .spanish = "", .english = ""},
        {.written = "_", .spanish = " ", .english = " "},
    }};
    const std::string text = std::move(text_);
    std::string out;
    out.reserve(text.size() + 16);
    std::size_t position = 0;
    while (position < text.size()) {
      if (text[position] == '#' && (position == 0 || text[position - 1] == '\n')) {
        while (position < text.size() && (text[position] == '#' || text[position] == ' '))
          ++position;
        continue;
      }
      const auto match = std::ranges::find_if(kSymbols, [&text, position](const Symbol& symbol) {
        return std::string_view(text).substr(position).starts_with(symbol.written);
      });
      if (match != kSymbols.end()) {
        out += spanish() ? match->spanish : match->english;
        position += match->written.size();
        continue;
      }
      out += text[position];
      ++position;
    }
    text_ = std::move(out);
  }

  static std::string collapseWhitespace(std::string_view text)
  {
    std::string out;
    out.reserve(text.size());
    std::size_t newlines = 0;
    bool pendingSpace = false;
    std::size_t position = 0;
    while (position < text.size()) {
      std::size_t width = 1;
      bool space = text[position] == ' ' || text[position] == '\t' || text[position] == '\r';
      if (text.substr(position).starts_with(kNoBreakSpace)) {
        space = true;
        width = kNoBreakSpace.size();
      }
      else if (text.substr(position).starts_with(kNarrowNoBreakSpace)) {
        space = true;
        width = kNarrowNoBreakSpace.size();
      }
      if (text[position] == '\n') {
        ++newlines;
        pendingSpace = false;
        position += 1;
        continue;
      }
      if (space) {
        pendingSpace = true;
        position += width;
        continue;
      }
      const char current = text[position];
      const bool closing = current == ',' || current == '.' || current == ';' || current == ':' || current == '!' ||
                           current == '?' || current == ')';
      if (newlines > 0 && !out.empty())
        out += newlines > 1 ? "\n\n" : "\n";
      else if (pendingSpace && !out.empty() && !closing)
        out += ' ';
      newlines = 0;
      pendingSpace = false;
      out += current;
      ++position;
    }
    return out;
  }

  std::string_view source_;
  SpeechLanguage language_;
  std::string text_;
};
}

SpeechLanguage speechLanguage(std::string_view code)
{
  if (code == "es")
    return SpeechLanguage::Spanish;
  if (code == "en")
    return SpeechLanguage::English;
  return SpeechLanguage::Other;
}

std::string normalizeSpeechText(std::string_view text, SpeechLanguage language)
{
  if (language == SpeechLanguage::Other)
    return std::string(text);
  return Normalizer(text, language).run();
}
