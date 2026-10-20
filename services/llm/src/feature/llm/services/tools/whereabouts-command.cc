#include "whereabouts-command.hxx"

#include <feature/llm/services/tools/app-command.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace
{
using Words = std::vector<std::string>;

constexpr std::size_t kMaxNameWords = 2;

constexpr std::array<std::string_view, 12> kPersonOpeners{
    "en que parte de la casa esta", "en que parte esta", "en que cuarto esta", "en que habitacion esta",
    "en que lugar esta", "donde se encuentra", "por donde anda", "donde andara", "donde anda", "donde esta",
    "where is", "where s"};

constexpr std::array<std::string_view, 6> kRoomOpeners{"quienes estan en", "quien esta en", "quien anda en",
                                                       "quien anda por", "quien hay en", "hay alguien en"};
constexpr std::array<std::string_view, 7> kRoomOpenersEn{"is there anyone in", "who is in", "who s in",
                                                         "is anyone in", "is anybody in", "is someone in",
                                                         "anyone in"};

constexpr std::array<std::string_view, 15> kHouseholdAsks{
    "quienes estan en casa", "quien esta en casa", "hay alguien en casa", "quien anda por casa",
    "quienes estan en la casa", "quien esta en la casa", "who is home", "who s home", "who is at home",
    "who s at home", "is anyone home", "is anyone at home", "is anybody home", "anyone home", "is anybody at home"};

constexpr std::array<std::string_view, 19> kRooms{
    "sala de estar", "living room", "storage room", "sala", "salon", "living", "lounge", "cocina", "kitchen",
    "oficina", "estudio", "despacho", "office", "study", "garaje", "garage", "cochera", "deposito", "storeroom"};

constexpr std::array<std::string_view, 8> kRoomArticles{"la", "el", "los", "las", "mi", "the", "my", "our"};

constexpr std::array<std::string_view, 26> kTails{
    "ahora", "ahorita", "mismo", "ya", "now", "right", "currently", "exactamente", "exactly", "please",
    "porfa", "por", "favor", "argus", "en", "este", "momento", "at", "the", "moment", "hoy", "today",
    "pues", "pe", "oye", "hey"};

constexpr std::array<std::string_view, 64> kNotNames{
    "el", "la", "los", "las", "lo", "un", "una", "mi", "mis", "tu", "tus", "su", "sus", "nuestro", "nuestra",
    "ese", "esa", "eso", "este", "esta", "esto", "aqui", "alli", "ahi", "alla", "alguien", "nadie", "todos",
    "que", "camara", "the", "a", "an", "my", "your", "his", "her", "our", "their", "it", "that", "this", "he",
    "she", "they", "everyone", "everybody", "someone", "somebody", "anyone", "anybody", "here", "there",
    "what", "camera", "mama", "papa", "mom", "dad", "hijo", "hija", "esposo", "esposa", "son"};

bool isAny(std::string_view word, std::span<const std::string_view> set)
{
  return std::ranges::find(set, word) != set.end();
}

Words split(std::string_view phrase)
{
  Words words;
  std::size_t at = 0;
  while (at < phrase.size()) {
    const std::size_t space = phrase.find(' ', at);
    const std::size_t end = space == std::string_view::npos ? phrase.size() : space;
    if (end > at)
      words.emplace_back(phrase.substr(at, end - at));
    at = end + 1;
  }
  return words;
}

struct PhraseAt
{
  const Words& words;
  std::size_t at{0};
  std::string_view phrase;
};

std::size_t matchedLength(const PhraseAt& probe)
{
  const Words needle = split(probe.phrase);
  if (needle.empty() || probe.at + needle.size() > probe.words.size())
    return 0;
  for (std::size_t index = 0; index < needle.size(); ++index)
    if (probe.words.at(probe.at + index) != needle.at(index))
      return 0;
  return needle.size();
}

struct Found
{
  std::size_t at{0};
  std::size_t size{0};
};

std::optional<Found> findPhrase(const Words& words, std::span<const std::string_view> phrases)
{
  for (std::size_t at = 0; at < words.size(); ++at)
    for (const std::string_view phrase : phrases)
      if (const std::size_t size = matchedLength({.words = words, .at = at, .phrase = phrase}); size > 0)
        return Found{.at = at, .size = size};
  return std::nullopt;
}

bool onlyTails(const Words& words, std::size_t from)
{
  for (std::size_t index = from; index < words.size(); ++index)
    if (!isAny(words.at(index), kTails))
      return false;
  return true;
}

bool nameWord(std::string_view word)
{
  return !word.empty() && !isAny(word, kNotNames) && !isAny(word, kTails) &&
         std::ranges::none_of(word, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string joined(const Words& words, Found span)
{
  std::string text;
  for (std::size_t index = span.at; index < span.at + span.size && index < words.size(); ++index)
    text += text.empty() ? words.at(index) : " " + words.at(index);
  return text;
}

std::optional<std::string> trailingName(const Words& words, std::size_t from)
{
  for (std::size_t size = 1; size <= kMaxNameWords && from + size <= words.size(); ++size) {
    bool names = true;
    for (std::size_t index = from; index < from + size; ++index)
      names = names && nameWord(words.at(index));
    if (!names)
      return std::nullopt;
    if (onlyTails(words, from + size))
      return joined(words, {.at = from, .size = size});
  }
  return std::nullopt;
}

tools::ToolCall whereaboutsCall()
{
  tools::ToolCall call;
  call.name = kWhereaboutsTool;
  call.arguments = Json::Value(Json::objectValue);
  return call;
}

std::optional<tools::ToolCall> personAsk(const Words& words)
{
  const auto opener = findPhrase(words, kPersonOpeners);
  if (!opener)
    return std::nullopt;
  const auto name = trailingName(words, opener->at + opener->size);
  if (!name)
    return std::nullopt;
  tools::ToolCall call = whereaboutsCall();
  call.arguments["person"] = *name;
  return call;
}

struct HomeShape
{
  std::string_view verb;
  std::span<const std::string_view> endings;
};

constexpr std::array<std::string_view, 2> kHomeEndingsEs{"en casa", "en la casa"};
constexpr std::array<std::string_view, 3> kHomeEndingsEn{"home", "at home", "in the house"};

std::optional<tools::ToolCall> homeAsk(const Words& words)
{
  for (const HomeShape shape : {HomeShape{.verb = "esta", .endings = kHomeEndingsEs},
                                HomeShape{.verb = "is", .endings = kHomeEndingsEn}}) {
    if (words.empty() || words.front() != shape.verb)
      continue;
    for (std::size_t size = 1; size <= kMaxNameWords && 1 + size < words.size(); ++size) {
      bool names = true;
      for (std::size_t index = 1; index <= size; ++index)
        names = names && nameWord(words.at(index));
      if (!names)
        break;
      for (const std::string_view ending : shape.endings) {
        const std::size_t matched = matchedLength({.words = words, .at = 1 + size, .phrase = ending});
        if (matched == 0 || !onlyTails(words, 1 + size + matched))
          continue;
        tools::ToolCall call = whereaboutsCall();
        call.arguments["person"] = joined(words, {.at = 1, .size = size});
        return call;
      }
    }
  }
  return std::nullopt;
}

std::optional<tools::ToolCall> roomAsk(const Words& words)
{
  auto opener = findPhrase(words, kRoomOpeners);
  if (!opener)
    opener = findPhrase(words, kRoomOpenersEn);
  if (!opener)
    return std::nullopt;
  std::size_t at = opener->at + opener->size;
  while (at < words.size() && isAny(words.at(at), kRoomArticles))
    ++at;
  for (const std::string_view room : kRooms) {
    const std::size_t matched = matchedLength({.words = words, .at = at, .phrase = room});
    if (matched == 0 || !onlyTails(words, at + matched))
      continue;
    tools::ToolCall call = whereaboutsCall();
    call.arguments["room"] = std::string(room);
    return call;
  }
  return std::nullopt;
}

std::optional<tools::ToolCall> householdAsk(const Words& words)
{
  const auto asked = findPhrase(words, kHouseholdAsks);
  if (!asked || !onlyTails(words, asked->at + asked->size))
    return std::nullopt;
  return whereaboutsCall();
}
}

std::optional<tools::ToolCall> whereaboutsCommandFor(const std::string& utterance)
{
  const Words words = spokenWords(utterance);
  if (words.empty())
    return std::nullopt;
  if (auto household = householdAsk(words))
    return household;
  if (auto room = roomAsk(words))
    return room;
  if (auto home = homeAsk(words))
    return home;
  return personAsk(words);
}
