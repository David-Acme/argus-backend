#include "speech.hxx"

namespace argus::mcp::speech
{

bool inEnglish(const ToolInvocation& invocation)
{
  return invocation.caller.lang == "en";
}

std::string_view languageOf(const ToolInvocation& invocation)
{
  return inEnglish(invocation) ? "en" : "es";
}

std::string say(const Words& words)
{
  return inEnglish(words.invocation) ? words.english : words.spanish;
}

ToolOutcome refuse(const Words& words, const std::string& code)
{
  return toolFailure({.text = say(words), .code = code});
}

std::string joined(const std::vector<std::string>& items, std::string_view language)
{
  std::string out;
  for (size_t index = 0; index < items.size(); ++index) {
    if (index > 0)
      out += index + 1 == items.size() ? (language == "en" ? " and " : " y ") : ", ";
    out += items[index];
  }
  return out;
}

}
