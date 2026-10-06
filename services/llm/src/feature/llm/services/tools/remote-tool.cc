#include "remote-tool.hxx"

#include <memory>
#include <utility>

namespace
{
argus::mcp::CallerContext callerOf(const tools::ToolCall& call)
{
  return {.userId = call.context.userId,
          .role = userRoleToString(call.context.role),
          .lang = call.context.lang,
          .sessionId = call.context.sessionId,
          .utterance = call.context.utterance,
          .decided = call.context.decided};
}

tools::ToolResult resultOf(const argus::mcp::ToolOutcome& outcome)
{
  tools::ToolResult result;
  result.ok = !outcome.isError;
  result.output = outcome.text;
  if (outcome.structured.isObject()) {
    result.data = outcome.structured;
    if (outcome.structured["code"].isString())
      result.code = outcome.structured["code"].asString();
  }
  return result;
}

tools::ToolResult dispatch(const RemoteToolInput& input, const tools::ToolCall& call)
{
  tools::ToolResult result;
  result.tool = call.name;
  const auto answer = input.client->callTool(
      {.name = input.spec.name, .arguments = call.arguments, .caller = callerOf(call)});
  if (!answer.value) {
    result.output = toolUnreachable(call.context.lang);
    result.code = "unavailable";
    return result;
  }
  const argus::mcp::ToolOutcome& outcome = *answer.value;
  result = resultOf(outcome);
  result.tool = call.name;
  if (!outcome.appAction || !result.ok)
    return result;
  if (!call.context.emitAction) {
    result.ok = false;
    result.output = appNotConnected(call.context.lang);
    result.code = "app_not_connected";
    return result;
  }
  call.context.emitAction(outcome.appAction->name, outcome.appAction->arguments);
  result.data = outcome.appAction->arguments;
  return result;
}
}

std::string_view appNotConnected(std::string_view lang)
{
  return lang == "en" ? "The app is not connected to this conversation."
                      : "La app no está conectada a esta conversación.";
}

std::string_view toolUnreachable(std::string_view lang)
{
  return lang == "en" ? "I could not reach that tool right now." : "No pude usar esa herramienta ahora.";
}

tools::ToolHandle remoteTool(const RemoteToolInput& input)
{
  return std::make_shared<const tools::ToolDescriptor>(
      tools::ToolDescriptor{.spec = input.spec,
                            .handler = [input](const tools::ToolCall& call) { return dispatch(input, call); }});
}
