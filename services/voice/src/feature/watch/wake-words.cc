#include "wake-words.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>

namespace
{

constexpr std::array kWakeForms = std::to_array<std::string_view>({"argus", "argos"});

constexpr std::array kInterjections = std::to_array<std::string_view>(
    {"oye", "oiga", "hey", "ey", "eh", "hola", "ok", "okay", "okey", "buenas", "buenos", "dias", "tardes",
     "noches", "hi", "hello", "bueno", "vale", "pues", "a", "ver", "disculpa", "perdon", "excuse", "me",
     "sorry", "porfa", "please", "che"});

constexpr std::array kTrailing = std::to_array<std::string_view>(
    {"y",   "o",    "u",     "e",    "pero",  "que",  "de",   "del",    "en",   "con",   "para",
     "por", "a",    "al",    "sin",  "sobre", "entre", "hasta", "desde", "porque", "cuando", "como",
     "el",  "la",   "los",   "las",  "un",    "una",  "mi",   "tu",     "su",   "and",   "or",
     "but", "to",   "of",    "with", "for",   "from", "about", "because", "the", "an",    "my",
     "your"});

constexpr std::array kClosing = std::to_array<std::string_view>(
    {"gracias argus", "gracias argos", "eso es todo", "eso seria todo", "thank you argus", "thanks argus",
     "that s all", "thats all", "that is all"});

constexpr std::array kHandoff = std::to_array<std::string_view>(
    {"pasalo al celular", "pasalo a mi celular", "pasa al celular", "sigue en mi celular", "seguimos en mi celular",
     "seguimos en el celular", "continua en mi celular", "continuemos en el celular", "pasalo al telefono",
     "sigue en mi telefono", "continue on my phone", "move it to my phone", "switch to my phone"});

constexpr std::array kPanic = std::to_array<std::string_view>(
    {"auxilio", "socorro", "ayuda ayuda", "llama a la policia", "llamen a la policia", "emergencia", "help me",
     "call the police", "emergency"});

constexpr std::array kDelegation = std::to_array<std::string_view>(
    {"puedes hablar con ella", "puedes hablar con el", "habla con ella", "habla con el", "respondele",
     "presentate", "contestale", "you can talk to her", "you can talk to him", "talk to her", "talk to him",
     "answer her", "answer him", "introduce yourself"});

std::optional<char> foldLatin(unsigned char second)
{
  switch (second) {
    case 0xA0:
    case 0xA1:
    case 0xA2:
    case 0xA4:
    case 0x80:
    case 0x81:
    case 0x82:
    case 0x84:
      return 'a';
    case 0xA8:
    case 0xA9:
    case 0xAA:
    case 0xAB:
    case 0x88:
    case 0x89:
    case 0x8A:
    case 0x8B:
      return 'e';
    case 0xAC:
    case 0xAD:
    case 0xAE:
    case 0xAF:
    case 0x8C:
    case 0x8D:
    case 0x8E:
    case 0x8F:
      return 'i';
    case 0xB2:
    case 0xB3:
    case 0xB4:
    case 0xB6:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x96:
      return 'o';
    case 0xB9:
    case 0xBA:
    case 0xBB:
    case 0xBC:
    case 0x99:
    case 0x9A:
    case 0x9B:
    case 0x9C:
      return 'u';
    case 0xB1:
    case 0x91:
      return 'n';
    case 0xA7:
    case 0x87:
      return 'c';
    default:
      return std::nullopt;
  }
}

size_t sequenceLength(unsigned char lead)
{
  if (lead < 0x80)
    return 1;
  if (lead >= 0xF0)
    return 4;
  if (lead >= 0xE0)
    return 3;
  if (lead >= 0xC0)
    return 2;
  return 1;
}

struct Scanned
{
  std::optional<char> letter;
  size_t length{1};
  bool comma{false};
};

Scanned scanAt(std::string_view text, size_t index)
{
  const auto byte = static_cast<unsigned char>(text[index]);
  if (byte < 0x80) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9'))
      return {.letter = static_cast<char>(byte), .length = 1, .comma = false};
    if (byte >= 'A' && byte <= 'Z')
      return {.letter = static_cast<char>(byte - 'A' + 'a'), .length = 1, .comma = false};
    return {.letter = std::nullopt, .length = 1, .comma = byte == ','};
  }
  const size_t length = std::min(sequenceLength(byte), text.size() - index);
  if (byte == 0xC3 && length == 2)
    return {.letter = foldLatin(static_cast<unsigned char>(text[index + 1])), .length = 2, .comma = false};
  return {.letter = std::nullopt, .length = std::max<size_t>(1, length), .comma = false};
}

bool isWordStart(std::string_view text, size_t index)
{
  const auto byte = static_cast<unsigned char>(text[index]);
  if (byte < 0x80)
    return std::isalnum(byte) != 0;
  return byte == 0xC3;
}

bool contains(std::span<const std::string_view> list, std::string_view word)
{
  return std::ranges::find(list, word) != list.end();
}

bool hasPhrase(std::string_view text, std::span<const std::string_view> phrases)
{
  const std::string padded = " " + foldText(text) + " ";
  return std::ranges::any_of(phrases, [&padded](std::string_view phrase) {
    return padded.find(" " + std::string(phrase) + " ") != std::string::npos;
  });
}

bool sentenceStart(std::string_view out)
{
  for (const char c : std::views::reverse(out)) {
    if (c == ' ' || c == '\t' || c == '\n')
      continue;
    return c == '.' || c == '!' || c == '?' || c == '\xBF' || c == '\xA1';
  }
  return true;
}

}

std::string foldText(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  bool space = true;
  for (size_t index = 0; index < text.size();) {
    const Scanned scanned = scanAt(text, index);
    index += scanned.length;
    if (scanned.letter) {
      out.push_back(*scanned.letter);
      space = false;
    }
    else if (!space) {
      out.push_back(' ');
      space = true;
    }
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

std::vector<WakeWord> wakeWordsOf(std::string_view text)
{
  std::vector<WakeWord> words;
  std::string current;
  bool comma = false;
  for (size_t index = 0; index < text.size();) {
    const Scanned scanned = scanAt(text, index);
    index += scanned.length;
    if (scanned.letter) {
      current.push_back(*scanned.letter);
      continue;
    }
    if (!current.empty()) {
      words.push_back({.folded = std::move(current), .afterComma = comma});
      current.clear();
      comma = false;
    }
    comma = comma || scanned.comma;
  }
  if (!current.empty())
    words.push_back({.folded = std::move(current), .afterComma = comma});
  return words;
}

int editDistance(std::string_view left, std::string_view right)
{
  std::vector<int> previous(right.size() + 1);
  std::vector<int> row(right.size() + 1);
  for (size_t column = 0; column <= right.size(); ++column)
    previous[column] = static_cast<int>(column);
  for (size_t line = 1; line <= left.size(); ++line) {
    row[0] = static_cast<int>(line);
    for (size_t column = 1; column <= right.size(); ++column) {
      const int substitution = previous[column - 1] + (left[line - 1] == right[column - 1] ? 0 : 1);
      row[column] = std::min({previous[column] + 1, row[column - 1] + 1, substitution});
    }
    std::swap(previous, row);
  }
  return previous[right.size()];
}

int wakeDistance(std::string_view folded)
{
  constexpr int kFar = 99;
  if (folded.size() < 4 || folded == "argu" || folded.starts_with("argum"))
    return kFar;
  int best = kFar;
  for (const std::string_view form : kWakeForms)
    best = std::min(best, editDistance(folded, form));
  if (folded.size() <= 4 && best > 1)
    return kFar;
  if (best >= 2 && !folded.starts_with('a'))
    return kFar;
  return best;
}

bool isWakeWord(std::string_view folded, int maxDistance)
{
  return wakeDistance(folded) <= maxDistance;
}

bool mentionsWake(const WakeRuleInput& input)
{
  return std::ranges::any_of(wakeWordsOf(input.text),
                             [&input](const WakeWord& word) { return isWakeWord(word.folded, input.maxDistance); });
}

WakeMatch matchWake(const WakeRuleInput& input)
{
  const std::vector<WakeWord> words = wakeWordsOf(input.text);
  const auto found = std::ranges::find_if(
      words, [&input](const WakeWord& word) { return isWakeWord(word.folded, input.maxDistance); });
  if (found == words.end())
    return {};
  const auto index = static_cast<size_t>(std::distance(words.begin(), found));
  const bool openingVocative = std::all_of(words.begin(), found, [](const WakeWord& word) {
    return contains(kInterjections, word.folded);
  });
  const bool trailingVocative = !openingVocative && found->afterComma && index + 1 == words.size();
  const bool commaVocative =
      !openingVocative && !trailingVocative && found->afterComma && index + 1 < words.size() &&
      words[index + 1].afterComma;
  WakeMatch match;
  match.distance = wakeDistance(found->folded);
  match.index = index;
  match.position = openingVocative || trailingVocative || commaVocative ? WakePosition::Vocative
                                                                        : WakePosition::Referential;
  const auto join = [](auto first, auto last) {
    std::string out;
    for (auto it = first; it != last; ++it) {
      if (!out.empty())
        out.push_back(' ');
      out += it->folded;
    }
    return out;
  };
  if (trailingVocative)
    match.request = join(words.begin(), found);
  else
    match.request = join(found + 1, words.end());
  return match;
}

float wakeConfidence(const WakeMatch& match)
{
  if (match.position != WakePosition::Vocative || match.distance < 0)
    return 0.0F;
  return std::max(0.0F, 1.0F - (0.25F * static_cast<float>(match.distance)));
}

std::string withoutSelfName(std::string_view text, VoiceLang lang)
{
  const std::string_view lower = lang == VoiceLang::En ? "your assistant" : "tu asistente";
  const std::string_view upper = lang == VoiceLang::En ? "Your assistant" : "Tu asistente";
  std::string out;
  out.reserve(text.size());
  for (size_t index = 0; index < text.size();) {
    if (!isWordStart(text, index)) {
      const Scanned scanned = scanAt(text, index);
      out.append(text.substr(index, scanned.length));
      index += scanned.length;
      continue;
    }
    size_t end = index;
    std::string folded;
    while (end < text.size() && isWordStart(text, end)) {
      const Scanned scanned = scanAt(text, end);
      if (scanned.letter)
        folded.push_back(*scanned.letter);
      end += scanned.length;
    }
    if (contains(kWakeForms, folded))
      out.append(sentenceStart(out) ? upper : lower);
    else
      out.append(text.substr(index, end - index));
    index = end;
  }
  return out;
}

bool closingPhrase(std::string_view text)
{
  return hasPhrase(text, kClosing);
}

bool handoffRequest(std::string_view text)
{
  return hasPhrase(text, kHandoff);
}

bool trailsOff(std::string_view text)
{
  const std::vector<WakeWord> words = wakeWordsOf(text);
  if (words.empty())
    return false;
  return contains(kTrailing, words.back().folded);
}

bool panicPhrase(std::string_view text)
{
  return hasPhrase(text, kPanic);
}

bool delegationPhrase(std::string_view text)
{
  return hasPhrase(text, kDelegation);
}
