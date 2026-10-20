#pragma once

#include "facet-lexicon.hxx"

#include <feature/llm/services/tools/tool-access.hxx>
#include <llm/llm-service.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ToolExecutor;

namespace turn
{

inline constexpr std::string_view kContextFetchChannel = "context_fetch";

struct ContextFetchInput
{
  const std::vector<ContextFacet>& facets;
  const std::vector<tools::ToolHandle>& offered;
  const ToolAudience& audience;
  const tools::ToolContext& context;
  const std::vector<std::string>& ran;
  std::vector<std::string> focus{};
};

using ContextFetch = std::function<std::vector<ContextFact>(const ContextFetchInput&)>;

struct ContextFetchLimits
{
  int64_t budgetMs{300};
  int maxLines{4};
  int maxChars{220};
  int agendaDays{2};
};

struct ToolContextFetchInput
{
  const ToolExecutor& executor;
  ContextFetchLimits limits{};
};

[[nodiscard]] ContextFetch toolContextFetch(const ToolContextFetchInput& input);

[[nodiscard]] std::optional<std::string_view> fetchToolFor(ContextFacet facet);

struct FactLinesInput
{
  const tools::ToolResult& result;
  int maxLines{4};
  int maxChars{220};
  std::vector<std::string> focus{};
};

[[nodiscard]] std::vector<std::string> factLines(const FactLinesInput& input);

}
