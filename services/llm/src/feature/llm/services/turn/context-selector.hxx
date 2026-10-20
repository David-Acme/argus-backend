#pragma once

#include "facet-lexicon.hxx"

#include <llm/llm-service.hxx>

#include <string>
#include <string_view>
#include <vector>

namespace turn
{

struct ContextInput
{
  std::string_view utterance;
  std::string_view lang;
  std::string_view tool{};
  std::string_view previousUser{};
  std::string_view previousAssistant{};
};

struct ContextChoice
{
  std::vector<ContextFacet> facets;
  std::vector<std::string> focus{};
};

class ContextSelector
{
public:
  [[nodiscard]] ContextChoice select(const ContextInput& input) const;
};

[[nodiscard]] std::size_t focusScore(std::string_view text, const std::vector<std::string>& focus);

[[nodiscard]] bool namesFocus(std::string_view text, const std::vector<std::string>& focus);

[[nodiscard]] std::vector<ContextFact> selectedFacts(const std::vector<ContextFact>& facts,
                                                     const ContextChoice& choice);

struct ContextBlockInput
{
  std::string_view lang;
  const std::vector<std::string>& facts;
};

[[nodiscard]] std::string contextBlock(const ContextBlockInput& input);

}
