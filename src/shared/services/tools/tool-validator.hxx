#pragma once

#include <optional>
#include <shared/contracts/tool-contracts.hxx>
#include <string>

namespace tools
{

// Argument schema check against ToolDescriptor::arguments. Returns an error
// message, or nullopt when the call is valid.
std::optional<std::string> validateArguments(const ToolDescriptor& descriptor,
                                             const ToolCall& call);

} // namespace tools
