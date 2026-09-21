#pragma once

#include <optional>
#include <shared/contracts/tool-contracts.hxx>
#include <string>

namespace tools
{

// Argument schema check against ToolDescriptor::arguments; nullopt when valid.
std::optional<std::string> validateArguments(const ToolDescriptor& descriptor,
                                             const ToolCall& call);

} // namespace tools
