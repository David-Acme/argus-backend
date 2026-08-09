#include "rule-parser.hxx"

#include <algorithm>
#include <cctype>
#include <shared/services/config-service/config-service.hxx>
#include <string>
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

} // namespace

std::optional<RuleParseResult> RuleParser::parse(const RuleParseInput& input)
{
  std::string text = input.text;
  auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return std::nullopt;
  text.erase(0, first);

  const std::string lowered = toLower(text);
  const auto phrases = ConfigService::getStringPairs(
      "memory.phrases." + (input.lang.empty() ? "es" : input.lang));

  const std::string* bestPhrase = nullptr;
  std::string bestType;
  for (const auto& [phrase, type] : phrases) {
    if (phrase.size() >= lowered.size())
      continue;
    if (lowered.compare(0, phrase.size(), phrase) != 0)
      continue;
    if (!bestPhrase || phrase.size() > bestPhrase->size()) {
      bestPhrase = &phrase;
      bestType = type;
    }
  }

  if (!bestPhrase)
    return std::nullopt;

  std::string content = text.substr(bestPhrase->size());
  auto contentFirst = content.find_first_not_of(" \t\r\n");
  if (contentFirst == std::string::npos)
    return std::nullopt;
  content.erase(0, contentFirst);
  if (content.empty())
    return std::nullopt;

  const MemoryType type = memoryTypeFromString(bestType);
  return RuleParseResult{.type = type,
                         .content = std::move(content),
                         .priority = priorityFor(type)};
}
