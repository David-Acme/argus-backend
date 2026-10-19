#include <feature/memory/services/memory/memory-scope-classifier.hxx>

#include <feature/memory/vocabulary/household-phrases.hxx>
#include <text/text-norm.hxx>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace
{
std::vector<std::string> tokensOf(std::string_view phrase)
{
  return text_norm::words(std::string(phrase), 1);
}

std::vector<std::vector<std::string>> clausesOf(std::string_view text)
{
  constexpr std::string_view kClauseEnds = ",.;:!?";
  std::vector<std::vector<std::string>> clauses;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = std::min(text.find_first_of(kClauseEnds, start), text.size());
    if (std::vector<std::string> words = tokensOf(text.substr(start, end - start)); !words.empty())
      clauses.push_back(std::move(words));
    start = end + 1;
  }
  return clauses;
}

bool endsWith(const std::vector<std::string>& words, const std::vector<std::string>& wanted)
{
  return wanted.size() <= words.size() &&
         std::equal(wanted.begin(), wanted.end(), words.end() - static_cast<std::ptrdiff_t>(wanted.size()));
}

struct CueHeard
{
  const std::vector<std::string>& utterance;
  const std::vector<std::vector<std::string>>& clauses;
};

bool cueMatches(const memory_vocabulary::HouseholdCue& cue, const CueHeard& heard)
{
  const std::vector<std::string> wanted = tokensOf(cue.phrase);
  if (wanted.empty() || wanted.size() > heard.utterance.size())
    return false;
  if (cue.mustEnd)
    return std::ranges::any_of(heard.clauses, [&wanted](const std::vector<std::string>& clause) { return endsWith(clause, wanted); });
  return std::ranges::search(heard.utterance, wanted).begin() != heard.utterance.end();
}

template <std::size_t N>
bool anyCue(const std::array<memory_vocabulary::HouseholdCue, N>& cues, const CueHeard& heard)
{
  return std::ranges::any_of(cues, [&heard](const memory_vocabulary::HouseholdCue& cue) { return cueMatches(cue, heard); });
}
}

bool saysForEveryone(std::string_view text, std::string_view lang)
{
  const std::vector<std::string> utterance = text_norm::words(std::string(text), 1);
  const std::vector<std::vector<std::string>> clauses = clausesOf(text);
  const CueHeard heard{.utterance = utterance, .clauses = clauses};
  if (lang == "en")
    return anyCue(memory_vocabulary::kHouseholdCuesEn, heard);
  return anyCue(memory_vocabulary::kHouseholdCuesEs, heard);
}

MemoryScope classifyMemoryScope(const MemoryScopeInput& input)
{
  if (saysForEveryone(input.text, input.lang))
    return MemoryScope::Household;
  return MemoryScope::Personal;
}
