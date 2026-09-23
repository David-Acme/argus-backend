#pragma once

#include <optional>
#include <llm/tool-contracts.hxx>
#include <string>

namespace tools
{

std::optional<std::string> validateArguments(const ToolDescriptor& descriptor,
                                             const ToolCall& call);

}
