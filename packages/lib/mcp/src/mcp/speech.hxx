#pragma once

#include <mcp/tool.hxx>

#include <string>
#include <string_view>
#include <vector>

namespace argus::mcp::speech
{

struct Words
{
  const ToolInvocation& invocation;
  std::string spanish;
  std::string english;
};

[[nodiscard]] bool inEnglish(const ToolInvocation& invocation);

[[nodiscard]] std::string_view languageOf(const ToolInvocation& invocation);

[[nodiscard]] std::string say(const Words& words);

[[nodiscard]] ToolOutcome refuse(const Words& words, const std::string& code);

[[nodiscard]] std::string joined(const std::vector<std::string>& items, std::string_view language);

}
