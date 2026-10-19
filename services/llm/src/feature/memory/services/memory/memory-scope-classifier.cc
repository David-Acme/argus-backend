#include <feature/memory/services/memory/memory-scope-classifier.hxx>

#include <feature/memory/vocabulary/household-phrases.hxx>
#include <text/text-norm.hxx>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace
{
std::vector<std::string> tokensOf(std::string_view phrase)
{
  return text_norm::words(std::string(phrase), 1);
}

bool cueMatches(const memory_vocabulary::HouseholdCue& cue, const std::vector<std::string>& utterance)
{
  const std::vector<std::string> wanted = tokensOf(cue.phrase);
  if (wanted.empty() || wanted.size() > utterance.size())
    return false;
  if (cue.mustEnd)
    return std::equal(wanted.begin(), wanted.end(), utterance.end() - static_cast<std::ptrdiff_t>(wanted.size()));
  return std::search(utterance.begin(), utterance.end(), wanted.begin(), wanted.end()) != utterance.end();
}
}

bool saysForEveryone(std::string_view text, std::string_view lang)
{
  const std::vector<std::string> utterance = text_norm::words(std::string(text), 1);
  if (lang == "en") {
    return std::any_of(memory_vocabulary::kHouseholdCuesEn.begin(), memory_vocabulary::kHouseholdCuesEn.end(),
                       [&](const memory_vocabulary::HouseholdCue& cue) { return cueMatches(cue, utterance); });
  }
  return std::any_of(memory_vocabulary::kHouseholdCuesEs.begin(), memory_vocabulary::kHouseholdCuesEs.end(),
                     [&](const memory_vocabulary::HouseholdCue& cue) { return cueMatches(cue, utterance); });
}

MemoryScope classifyMemoryScope(const MemoryScopeInput& input)
{
  if (saysForEveryone(input.text, input.lang))
    return MemoryScope::Household;
  return MemoryScope::Personal;
}
