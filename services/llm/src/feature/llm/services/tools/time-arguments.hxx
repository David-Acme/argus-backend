#pragma once

#include <mcp/tool.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace time_arguments
{

struct NormalizeInput
{
  tools::ToolCall& call;
  const argus::mcp::ToolSpec& spec;
  int64_t now{0};
};

[[nodiscard]] bool needsClock(const argus::mcp::ToolSpec& spec);

[[nodiscard]] std::string clockLine(int64_t now, const std::string& lang);

void normalize(const NormalizeInput& input);

}
