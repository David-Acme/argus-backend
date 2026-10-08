#pragma once

#include <text/name-match.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace call_checks
{

inline constexpr std::array<std::string_view, 7> kWeekdaysEs{"lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo"};
inline constexpr std::array<std::string_view, 12> kMonthsEs{"enero", "febrero", "marzo", "abril", "mayo", "junio",
                                                            "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
inline constexpr std::array<std::string_view, 7> kWeekdaysEn{"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
inline constexpr std::array<std::string_view, 12> kMonthsEn{"January", "February", "March", "April", "May", "June",
                                                            "July", "August", "September", "October", "November", "December"};

inline constexpr std::array<std::string_view, 10> kInterjections{"hola", "buenas", "hey", "hi", "hello",
                                                                 "claro", "vale", "ok", "gracias", "good"};
inline constexpr std::array<std::string_view, 6> kGreetingTokens{"hola", "buenas", "hey", "hi", "hello", "good"};
struct StaticPromptInput
{
  const std::optional<std::string>& roles;
  const std::string& rolesDefault;
  const std::string& prompt;
  const std::string& person;
  const std::string& known;
  const std::vector<std::string>& notes;
};

inline std::string staticPrompt(const StaticPromptInput& input)
{
  const std::string& roles = input.roles ? *input.roles : input.rolesDefault;
  std::string content;
  if (!roles.empty()) {
    content += roles;
    content += "\n\n";
  }
  content += input.prompt;
  if (!input.person.empty()) {
    content += "\n\n";
    content += input.person;
  }
  if (!input.notes.empty()) {
    content += '\n';
    content += input.known;
    for (const auto& note : input.notes) {
      content += '\n';
      content += note;
    }
  }
  return content;
}
inline constexpr std::array<std::string_view, 4> kSystemRecitals{"información del sistema", "system information",
                                                                 "lo que sabes de la persona", "what you know about the person"};
inline constexpr std::array<std::string_view, 5> kNameAsks{"como te llamas", "como se llama", "tu nombre",
                                                           "your name", "whats your name"};

struct NameAskInput
{
  const std::string& reply;
  bool nameUnknown{false};
  bool firstTurn{false};
};

inline std::vector<std::string> wordsOf(const std::string& text)
{
  const std::string folded = text_norm::folded(text);
  std::vector<std::string> words;
  size_t at = 0;
  while (at < folded.size()) {
    const size_t end = std::min(folded.find(' ', at), folded.size());
    if (end > at)
      words.push_back(folded.substr(at, end - at));
    at = end + 1;
  }
  return words;
}

inline bool hasWord(const std::vector<std::string>& words, std::string_view name)
{
  return std::ranges::find(words, text_norm::folded(std::string(name))) != words.end();
}

inline bool containsWord(const std::string& text, std::string_view name)
{
  size_t at = text.find(name);
  while (at != std::string::npos) {
    const bool leftFree = at == 0 || std::isalnum(static_cast<unsigned char>(text[at - 1])) == 0;
    const size_t end = at + name.size();
    const bool rightFree = end >= text.size() || std::isalnum(static_cast<unsigned char>(text[end])) == 0;
    if (leftFree && rightFree)
      return true;
    at = text.find(name, at + 1);
  }
  return false;
}

inline bool namesEnglishDate(const std::string& reply)
{
  const auto titled = [&reply](const auto& names) {
    return std::ranges::any_of(names, [&reply](std::string_view name) { return containsWord(reply, name); });
  };
  return titled(kWeekdaysEn) || titled(kMonthsEn);
}

inline bool namesADate(const std::string& reply)
{
  const std::vector<std::string> words = wordsOf(reply);
  const auto foldedAnywhere = [&words](const auto& names) {
    return std::ranges::any_of(names, [&words](std::string_view name) { return hasWord(words, name); });
  };
  return foldedAnywhere(kWeekdaysEs) || foldedAnywhere(kMonthsEs) || namesEnglishDate(reply);
}

inline bool isInterjection(std::string_view word)
{
  return std::ranges::find(kInterjections, word) != kInterjections.end();
}

inline size_t terminatorAt(const std::string& text, size_t at)
{
  const char c = text[at];
  if (c == '.' || c == '!' || c == '?' || c == '\n')
    return 1;
  if (at + 2 <= text.size() && (text.compare(at, 2, "\xC2\xA1") == 0 || text.compare(at, 2, "\xC2\xBF") == 0))
    return 2;
  if (at + 3 <= text.size() && text.compare(at, 3, "\xE2\x80\xA6") == 0)
    return 3;
  return 0;
}

inline int sentenceCount(const std::string& reply)
{
  int count = 0;
  const auto close = [&reply, &count](size_t from, size_t to) {
    const std::string segment = reply.substr(from, to - from);
    if (segment.find_first_not_of(" \t\r\n") == std::string::npos)
      return;
    const std::vector<std::string> words = wordsOf(segment);
    if (!words.empty() && words.size() <= 3 && isInterjection(words.front()))
      return;
    ++count;
  };
  size_t start = 0;
  size_t at = 0;
  while (at < reply.size()) {
    const size_t width = terminatorAt(reply, at);
    if (width == 0) {
      ++at;
      continue;
    }
    close(start, at);
    at += width;
    start = at;
  }
  close(start, reply.size());
  return count;
}

inline bool opensWithGreeting(const std::string& reply)
{
  const std::vector<std::string> words = wordsOf(reply);
  if (words.empty())
    return false;
  return std::ranges::find(kGreetingTokens, words.front()) != kGreetingTokens.end();
}

inline bool parrots(const std::string& reply, const std::vector<std::string>& examples)
{
  if (examples.empty())
    return false;
  const std::string heard = text_norm::folded(reply);
  const std::vector<std::string> heardWords = wordsOf(reply);
  for (const auto& example : examples) {
    const std::string foldedExample = text_norm::folded(example);
    if (foldedExample.empty())
      continue;
    if (heard.find(foldedExample) != std::string::npos)
      return true;
    std::vector<std::string> exampleTokens;
    for (const auto& token : wordsOf(example))
      if (std::ranges::find(exampleTokens, token) == exampleTokens.end())
        exampleTokens.push_back(token);
    if (exampleTokens.empty())
      continue;
    const auto hits = static_cast<size_t>(std::ranges::count_if(exampleTokens, [&heardWords](const std::string& token) {
      return std::ranges::find(heardWords, token) != heardWords.end();
    }));
    if (static_cast<double>(hits) / static_cast<double>(exampleTokens.size()) > 0.8)
      return true;
  }
  return false;
}

inline bool missesExpectedToken(const std::string& reply, const std::vector<std::string>& expected)
{
  if (expected.empty())
    return false;
  const std::vector<std::string> words = wordsOf(reply);
  return std::ranges::none_of(expected, [&words](const std::string& token) {
    return std::ranges::find(words, text_norm::folded(token)) != words.end();
  });
}

inline bool asksName(const std::string& reply)
{
  const std::string folded = text_norm::folded(reply);
  return std::ranges::any_of(kNameAsks, [&folded](std::string_view phrase) { return folded.find(phrase) != std::string::npos; });
}

inline bool missedNameAsk(const NameAskInput& input)
{
  return input.nameUnknown && input.firstTurn && !asksName(input.reply);
}

inline bool nameAskRepeated(const NameAskInput& input)
{
  return !input.firstTurn && asksName(input.reply);
}

inline bool recitesNotes(const std::string& reply, const std::vector<std::string>& notes)
{
  const std::string heard = text_norm::folded(reply);
  for (const auto& note : notes) {
    const std::string recited = text_norm::folded(note);
    if (!recited.empty() && heard.find(recited) != std::string::npos)
      return true;
  }
  return std::ranges::any_of(kSystemRecitals, [&heard](std::string_view phrase) {
    const std::string recited = text_norm::folded(std::string(phrase));
    return !recited.empty() && heard.find(recited) != std::string::npos;
  });
}

}
