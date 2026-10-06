#pragma once

#include <shared/vocabulary/tool-contracts.hxx>

#include <string>
#include <string_view>
#include <vector>

namespace tool_policy
{

struct PolicyInput
{
  const std::vector<tools::ToolHandle>& tools;
  std::string_view lang;
};

struct PromptInput
{
  const std::vector<tools::ToolHandle>& tools;
  std::string_view lang;
  bool clientActions{false};
};

[[nodiscard]] std::string generated(const PolicyInput& input);

[[nodiscard]] std::string systemPrompt(const PromptInput& input);

}
