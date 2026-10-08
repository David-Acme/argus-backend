#include "context-selector.hxx"

#include <algorithm>

namespace turn
{

ContextChoice ContextSelector::select(const ContextInput& input) const
{
  ContextChoice choice;
  if (const auto implied = facetForTool(input.tool))
    choice.facets.push_back(*implied);
  for (const ContextFacet facet : facetsInText(input.utterance, input.lang))
    if (std::ranges::find(choice.facets, facet) == choice.facets.end())
      choice.facets.push_back(facet);
  return choice;
}

std::vector<ContextFact> selectedFacts(const std::vector<ContextFact>& facts, const ContextChoice& choice)
{
  std::vector<ContextFact> out;
  for (const ContextFacet facet : choice.facets)
    for (const ContextFact& fact : facts)
      if (contextFacetFromString(fact.facet) == facet)
        out.push_back(fact);
  return out;
}

}
