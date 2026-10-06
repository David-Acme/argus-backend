#pragma once

#include <mcp/speech.hxx>
#include <mcp/tool.hxx>

#include <cstdint>
#include <errors/validation-exception.hxx>
#include <json/value.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace productivity_tools
{

struct SpokenTime
{
  int64_t epoch{0};
  bool allDay{false};
};

struct IdempotencyInput
{
  std::string scope;
  int64_t userId{0};
  std::string subject;
  int64_t at{0};
};

[[nodiscard]] std::string spokenWhen(const SpokenTime& when, std::string_view lang);

[[nodiscard]] std::string idempotencyKey(const IdempotencyInput& input);

[[nodiscard]] std::optional<int64_t> timeArgument(const Json::Value& arguments, const char* name);

[[nodiscard]] argus::mcp::ToolOutcome invalid(const ValidationException& error,
                                              const argus::mcp::ToolInvocation& invocation);

[[nodiscard]] argus::mcp::ToolOutcome unknownCaller(const argus::mcp::ToolInvocation& invocation);

}
