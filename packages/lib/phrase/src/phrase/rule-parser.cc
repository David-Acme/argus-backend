#include "rule-parser.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <phrase/phrase-catalog.hxx>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

int priorityFor(MemoryType type)
{
  switch (type) {
    case MemoryType::Instruction:
      return 90;
    case MemoryType::Episodic:
      return 70;
    default:
      return 85;
  }
}

std::string toLower(const std::string& s)
{
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

bool isBoundary(char c)
{
  return c == ' ' || c == ',' || c == '.' || c == ';' || c == ':';
}

bool openerOk(const std::string& lowered, uint32_t begin)
{
  return begin == 0 || isBoundary(lowered[begin - 1]);
}

bool closerOk(const std::string& lowered, uint32_t end)
{
  return end >= lowered.size() || isBoundary(lowered[end]) ||
         lowered[end] == '?';
}

size_t wordsBefore(const std::string& lowered, size_t anchorPos)
{
  size_t words = 0;
  size_t i = 0;
  while (i < anchorPos) {
    while (i < anchorPos && (lowered[i] == ' ' || lowered[i] == ','))
      ++i;
    if (i >= anchorPos)
      break;
    ++words;
    while (i < anchorPos && lowered[i] != ' ')
      ++i;
  }
  return words;
}

size_t wordsAfter(const std::string& lowered, size_t anchorPos)
{
  size_t words = 0;
  size_t i = anchorPos;
  while (i < lowered.size()) {
    while (i < lowered.size() && (lowered[i] == ' ' || lowered[i] == ','))
      ++i;
    if (i >= lowered.size())
      break;
    ++words;
    while (i < lowered.size() && lowered[i] != ' ')
      ++i;
  }
  return words;
}

bool isNearStart(const std::string& lowered, size_t anchorPos)
{
  return wordsBefore(lowered, anchorPos) < 5;
}

struct HitFilter
{
  PhraseKind kind;
  bool requireCloser = false;
  bool requireNearStart = false;
};

size_t tagCut(const std::string& lowered, uint32_t begin)
{
  size_t i = begin;
  while (i > 0 && lowered[i - 1] == ' ')
    --i;
  if (i == 0)
    return 0;
  const char prev = lowered[i - 1];
  if (prev == ',' || prev == ';' || prev == '.' || prev == ':')
    return i - 1;
  return std::string::npos;
}

bool insideTrigger(const std::vector<PhraseHit>& hits, const PhraseHit& word)
{
  for (const auto& hit : hits) {
    if (hit.kind != PhraseKind::Trigger)
      continue;
    if (hit.begin <= word.begin && word.end <= hit.end &&
        (hit.end - hit.begin) > (word.end - word.begin))
      return true;
  }
  return false;
}

size_t clauseStartBefore(const std::string& lowered, size_t until)
{
  if (until == 0)
    return 0;
  const size_t cut = lowered.find_last_of(",;:.!", until - 1);
  size_t at = cut == std::string::npos ? 0 : cut + 1;
  while (at < until && lowered[at] == ' ')
    ++at;
  return at;
}

bool isCliticImperative(std::string_view word)
{
  static constexpr std::array<std::string_view, 6> kMemoryVerbs{
      "recu\xc3\xa9rd", "acu\xc3\xa9rd", "ap\xc3\xbant", "an\xc3\xb3t", "gu\xc3\xa1rd", "memor\xc3\xad"};
  if (std::ranges::any_of(kMemoryVerbs, [word](std::string_view verb) { return word.starts_with(verb); }))
    return false;
  static constexpr std::array<std::string_view, 5> kAccented{"\xc3\xa1", "\xc3\xa9", "\xc3\xad",
                                                             "\xc3\xb3", "\xc3\xba"};
  const bool accented = std::ranges::any_of(
      kAccented, [word](std::string_view vowel) { return word.find(vowel) != std::string_view::npos; });
  return accented && word.size() > 5 && (word.ends_with("me") || word.ends_with("nos"));
}

struct ClauseOpeningInput
{
  const std::vector<PhraseHit>& hits;
  const std::string& lowered;
  size_t clauseStart;
  size_t limit;
};

bool opensWithCommand(const ClauseOpeningInput& input)
{
  const std::string& lowered = input.lowered;
  size_t at = input.clauseStart;
  for (;;) {
    if (at >= input.limit)
      return false;
    const auto startsHere = [&](PhraseKind kind) {
      size_t end = 0;
      for (const auto& hit : input.hits) {
        if (hit.kind == kind && hit.begin == at && closerOk(lowered, hit.end))
          end = std::max(end, static_cast<size_t>(hit.end));
      }
      return end;
    };
    if (startsHere(PhraseKind::Command) > 0)
      return true;
    const size_t wordEnd = std::min(lowered.find(' ', at), lowered.size());
    if (isCliticImperative(std::string_view(lowered).substr(at, wordEnd - at)))
      return true;
    const size_t filler = startsHere(PhraseKind::Filler);
    if (filler == 0)
      return false;
    at = filler;
    while (at < input.limit && (lowered[at] == ' ' || lowered[at] == ','))
      ++at;
  }
}

struct BestHitInput
{
  const std::vector<PhraseHit>& hits;
  const std::string& lowered;
  HitFilter filter;
};

const PhraseHit* bestHit(const BestHitInput& input)
{
  const std::vector<PhraseHit>& hits = input.hits;
  const std::string& lowered = input.lowered;
  const HitFilter& filter = input.filter;

  const PhraseHit* best = nullptr;
  for (const auto& hit : hits) {
    if (hit.kind != filter.kind)
      continue;
    if (!openerOk(lowered, hit.begin))
      continue;
    if (filter.requireCloser && !closerOk(lowered, hit.end))
      continue;
    if (filter.requireNearStart && !isNearStart(lowered, hit.begin))
      continue;
    if (!best || (hit.end - hit.begin) > (best->end - best->begin))
      best = &hit;
  }
  return best;
}

}

std::string RuleParser::stripTrailingConfirmation(std::string text,
                                                  const std::string& lang) const
{
  for (;;) {
    const size_t end = text.find_last_not_of(" \t\r\n");
    if (end == std::string::npos)
      return text;
    text.erase(end + 1);

    const std::string lowered = toLower(text);
    const std::vector<PhraseHit> hits = catalog_.match(lowered, lang);
    size_t cut = std::string::npos;
    for (const auto& hit : hits) {
      const bool tail = hit.kind == PhraseKind::Confirmation ||
                        hit.kind == PhraseKind::Filler;
      if (!tail || hit.end != lowered.size())
        continue;
      const size_t at = tagCut(lowered, hit.begin);
      if (at == std::string::npos)
        continue;
      if (cut == std::string::npos || at < cut)
        cut = at;
    }
    if (cut != std::string::npos) {
      text.erase(cut);
      continue;
    }
    const size_t q = text.find_last_not_of("?¿!");
    if (q == std::string::npos || q + 1 >= text.size())
      return text;
    text.erase(q + 1);
  }
}

bool RuleParser::isRecallTalk(const std::string& lowered,
                              const std::string& lang) const
{
  const std::vector<PhraseHit> hits = catalog_.match(lowered, lang);
  for (const auto& hit : hits) {
    if (hit.kind == PhraseKind::RecallMarker)
      return true;
  }
  return false;
}

bool RuleParser::isQuestion(const RuleParseInput& input) const
{
  if (input.text.find('?') != std::string::npos ||
      input.text.find("\xc2\xbf") != std::string::npos)
    return true;

  const std::string lowered = toLower(input.text);
  if (isRecallTalk(lowered, input.lang))
    return true;

  const std::vector<PhraseHit> hits = catalog_.match(lowered, input.lang);
  for (const auto& hit : hits) {
    if (hit.kind != PhraseKind::Interrogative)
      continue;
    if (!openerOk(lowered, hit.begin) || !closerOk(lowered, hit.end))
      continue;
    if (insideTrigger(hits, hit))
      continue;
    if (wordsBefore(lowered, hit.begin) < 3)
      return true;
    const std::string_view word(lowered.data() + hit.begin,
                                hit.end - hit.begin);
    if (word != "que" && word != "qué" && wordsAfter(lowered, hit.end) <= 3)
      return true;
  }
  return false;
}

bool RuleParser::isCommand(const RuleParseInput& input) const
{
  const std::string lowered = toLower(input.text);
  const auto first = lowered.find_first_not_of(" ,");
  if (first == std::string::npos)
    return false;
  const std::vector<PhraseHit> hits = catalog_.match(lowered, input.lang);
  return opensWithCommand({.hits = hits, .lowered = lowered, .clauseStart = first, .limit = lowered.size()});
}

bool RuleParser::isCancellation(const RuleParseInput& input) const
{
  const std::string lowered = toLower(input.text);
  const std::vector<PhraseHit> hits = catalog_.match(lowered, input.lang);
  const PhraseHit* best =
      bestHit({.hits = hits,
               .lowered = lowered,
               .filter = {.kind = PhraseKind::Cancellation,
                          .requireCloser = true,
                          .requireNearStart = false}});
  return best != nullptr;
}

bool RuleParser::isVacuous(const RuleParseInput& input) const
{
  const std::string text = stripTrailingConfirmation(
      stripFillers(input), input.lang);
  if (text.find_first_not_of(" \t\r\n,.;:!?") == std::string::npos)
    return true;
  if (isFiller(text, input.lang))
    return true;

  size_t words = 0;
  size_t chars = 0;
  bool inWord = false;
  for (const unsigned char c : text) {
    if ((c & 0xC0) == 0x80)
      continue;
    ++chars;
    if (c == ' ')
      inWord = false;
    else if (!inWord) {
      inWord = true;
      ++words;
    }
  }
  return words < 2 || chars < 6;
}

std::string RuleParser::stripFillers(const RuleParseInput& input) const
{
  std::string text = input.text;
  for (;;) {
    const auto first = text.find_first_not_of(" \t\r\n,.;:");
    if (first == std::string::npos)
      return {};
    if (first > 0)
      text.erase(0, first);

    const std::string lowered = toLower(text);
    const std::vector<PhraseHit> hits = catalog_.match(lowered, input.lang);
    size_t cut = 0;
    for (const auto& hit : hits) {
      if (hit.kind != PhraseKind::Filler || hit.begin != 0)
        continue;
      if (!closerOk(lowered, hit.end))
        continue;
      cut = std::max(cut, static_cast<size_t>(hit.end));
    }
    if (cut == 0)
      return text;
    text.erase(0, cut);
  }
}

bool RuleParser::isFiller(const std::string& phrase,
                          const std::string& lang) const
{
  const std::string lowered = toLower(phrase);
  if (lowered.empty())
    return true;
  const std::vector<PhraseHit> hits = catalog_.match(lowered, lang);
  for (const auto& hit : hits) {
    const bool spansAll = hit.begin == 0 && hit.end == lowered.size();
    if (!spansAll)
      continue;
    if (hit.kind == PhraseKind::Filler || hit.kind == PhraseKind::Trigger ||
        hit.kind == PhraseKind::Interrogative)
      return true;
  }
  return false;
}

std::optional<std::string>
RuleParser::contentBeforeTrigger(const ContentBeforeTriggerInput& input) const
{
  const std::string& text = input.text;
  const std::string& lowered = input.lowered;
  const uint32_t begin = input.begin;
  const uint32_t end = input.end;

  if (begin == 0)
    return std::nullopt;
  if (lowered.find_first_not_of(" \t\r\n,.;:!", end) != std::string::npos)
    return std::nullopt;

  std::string before = text.substr(0, begin);
  while (!before.empty() && (before.back() == ' ' || before.back() == ',' ||
                             before.back() == '.' || before.back() == ';'))
    before.pop_back();
  if (before.empty())
    return std::nullopt;
  return before;
}

std::optional<RuleParseResult>
RuleParser::parse(const RuleParseInput& input) const
{
  const std::string& text = input.text;
  const std::string lowered = toLower(text);

  const std::vector<PhraseHit> hits = catalog_.match(lowered, input.lang);

  const PhraseHit* best =
      bestHit({.hits = hits,
               .lowered = lowered,
               .filter = {.kind = PhraseKind::Trigger,
                          .requireCloser = true,
                          .requireNearStart = false}});
  if (!best)
    return std::nullopt;

  if (auto trailing = contentBeforeTrigger({.text = text,
                                            .lowered = lowered,
                                            .begin = best->begin,
                                            .end = best->end})) {
    RuleParseResult result;
    result.type = best->memoryType;
    result.content = std::move(*trailing);
    result.priority = priorityFor(best->memoryType);
    return result;
  }

  std::string content = text.substr(best->end);
  const auto contentFirst = content.find_first_not_of(" \t\r\n");
  if (contentFirst == std::string::npos)
    return std::nullopt;
  content.erase(0, contentFirst);
  content = stripTrailingConfirmation(std::move(content), input.lang);
  if (content.empty())
    return std::nullopt;

  return RuleParseResult{.type = best->memoryType,
                         .content = std::move(content),
                         .priority = priorityFor(best->memoryType)};
}

std::optional<RuleParseResult>
RuleParser::parseStatement(const RuleParseInput& input) const
{
  if (input.text.find('?') != std::string::npos ||
      input.text.find("\xc2\xbf") != std::string::npos)
    return std::nullopt;

  const std::string text = stripTrailingConfirmation(input.text, input.lang);
  if (text.size() < 15)
    return std::nullopt;

  const std::string lowered = toLower(text);
  if (isRecallTalk(lowered, input.lang))
    return std::nullopt;

  std::vector<PhraseHit> hits = catalog_.match(lowered, input.lang);
  std::vector<PhraseHit> openers;
  std::ranges::copy_if(hits, std::back_inserter(openers), [](const PhraseHit& hit) {
    return hit.kind == PhraseKind::Command || hit.kind == PhraseKind::Filler;
  });
  std::erase_if(hits, [&](const PhraseHit& hit) {
    return hit.kind == PhraseKind::StatementStart &&
           opensWithCommand({.hits = openers,
                             .lowered = lowered,
                             .clauseStart = clauseStartBefore(lowered, hit.begin),
                             .limit = hit.begin});
  });

  const PhraseHit* best =
      bestHit({.hits = hits,
               .lowered = lowered,
               .filter = {.kind = PhraseKind::StatementStart,
                          .requireCloser = false,
                          .requireNearStart = true}});
  if (!best)
    return std::nullopt;

  size_t from = best->begin;
  if (from >= 2 && lowered.compare(from - 2, 2, "a ") == 0 &&
      (from == 2 || lowered[from - 3] == ' ' || lowered[from - 3] == ','))
    from -= 2;
  std::string content = text.substr(from);
  const auto first = content.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return std::nullopt;
  content.erase(0, first);

  return RuleParseResult{.type = best->memoryType,
                         .content = std::move(content),
                         .priority = priorityFor(best->memoryType)};
}
