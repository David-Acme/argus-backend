#pragma once

#include "facet-lexicon.hxx"

#include <llm/llm-service.hxx>

#include <string_view>
#include <vector>

namespace turn
{

struct ContextInput
{
  std::string_view utterance;
  std::string_view lang;
  std::string_view tool{};
};

struct ContextChoice
{
  std::vector<ContextFacet> facets;
};

class ContextSelector
{
public:
  [[nodiscard]] ContextChoice select(const ContextInput& input) const;
};

[[nodiscard]] std::vector<ContextFact> selectedFacts(const std::vector<ContextFact>& facts,
                                                     const ContextChoice& choice);

}
