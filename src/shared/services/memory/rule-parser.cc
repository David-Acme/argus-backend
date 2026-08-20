#include "rule-parser.hxx"

#include <algorithm>
#include <cctype>
#include <shared/services/memory/phrase-catalog.hxx>
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

// "…el pescado, está bien" is a tag; "…el router está bien" is the fact. Only
// a confirmation detached by punctuation may be cut, and the punctuation goes
// with it. npos = the phrase belongs to the sentence, leave it alone.
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

const PhraseHit* bestHit(const std::vector<PhraseHit>& hits,
                         const std::string& lowered, const HitFilter& filter)
{
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

} // namespace

std::string RuleParser::stripTrailingConfirmation(std::string text,
                                                  const std::string& lang) const
{
  thread_local std::vector<PhraseHit> hits;
  for (;;) {
    const size_t end = text.find_last_not_of(" \t\r\n");
    if (end == std::string::npos)
      return text;
    text.erase(end + 1);

    const std::string lowered = toLower(text);
    catalog_.match(lowered, lang, hits);
    size_t cut = std::string::npos;
    for (const auto& hit : hits) {
      // A phrase carries a single kind, so "vale" is a Filler while "bien" is
      // a Confirmation. Detached at the tail, both are noise.
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
    // "…ok?", "…verdad?" — speech keeps the mark after the tag.
    const size_t q = text.find_last_not_of("?¿!");
    if (q == std::string::npos || q + 1 >= text.size())
      return text;
    text.erase(q + 1);
  }
}

bool RuleParser::isRecallTalk(const std::string& lowered,
                              const std::string& lang) const
{
  thread_local std::vector<PhraseHit> hits;
  catalog_.match(lowered, lang, hits);
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

  thread_local std::vector<PhraseHit> hits;
  catalog_.match(lowered, input.lang, hits);
  for (const auto& hit : hits) {
    if (hit.kind != PhraseKind::Interrogative)
      continue;
    if (!openerOk(lowered, hit.begin) || !closerOk(lowered, hit.end))
      continue;
    // The "que" of "recuerda que" is a relative pronoun, not a question.
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

bool RuleParser::isCancellation(const RuleParseInput& input) const
{
  const std::string lowered = toLower(input.text);
  thread_local std::vector<PhraseHit> hits;
  catalog_.match(lowered, input.lang, hits);
  const PhraseHit* best =
      bestHit(hits, lowered,
              {.kind = PhraseKind::Cancellation,
               .requireCloser = true,
               .requireNearStart = false});
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
  thread_local std::vector<PhraseHit> hits;
  for (;;) {
    const auto first = text.find_first_not_of(" \t\r\n,.;:");
    if (first == std::string::npos)
      return {};
    if (first > 0)
      text.erase(0, first);

    const std::string lowered = toLower(text);
    catalog_.match(lowered, input.lang, hits);
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
  thread_local std::vector<PhraseHit> hits;
  catalog_.match(lowered, lang, hits);
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
RuleParser::contentBeforeTrigger(const std::string& text,
                                 const std::string& lowered, uint32_t begin,
                                 uint32_t end) const
{
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

  thread_local std::vector<PhraseHit> hits;
  catalog_.match(lowered, input.lang, hits);

  const PhraseHit* best =
      bestHit(hits, lowered,
              {.kind = PhraseKind::Trigger, .requireCloser = true});
  if (!best)
    return std::nullopt;

  if (auto trailing =
          contentBeforeTrigger(text, lowered, best->begin, best->end)) {
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

  thread_local std::vector<PhraseHit> hits;
  catalog_.match(lowered, input.lang, hits);

  const PhraseHit* best =
      bestHit(hits, lowered,
              {.kind = PhraseKind::StatementStart, .requireNearStart = true});
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
