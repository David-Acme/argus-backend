#include "tool-executor.hxx"

#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/module-offer.hxx>
#include <feature/llm/services/tools/remote-tool.hxx>
#include <feature/llm/services/tools/time-arguments.hxx>

#include <mcp/schema.hxx>

#include <algorithm>
#include <ctime>
#include <utility>

namespace
{
constexpr std::string_view kEnableTool = "modules.enable";

void dropUndeclared(const Json::Value& schema, Json::Value& arguments)
{
  const Json::Value& extra = schema["additionalProperties"];
  const Json::Value& properties = schema["properties"];
  if (!arguments.isObject() || !extra.isBool() || extra.asBool())
    return;
  for (const auto& name : arguments.getMemberNames())
    if (!properties.isObject() || !properties.isMember(name))
      arguments.removeMember(name);
}

tools::ToolResult refused(const tools::ToolCall& call, std::string code, std::string output)
{
  tools::ToolResult result;
  result.tool = call.name;
  result.code = std::move(code);
  result.output = std::move(output);
  return result;
}
}

void ToolExecutor::attachLedger(std::shared_ptr<tools::IntentLedger> ledger)
{
  ledger_ = std::move(ledger);
}

std::vector<tools::ToolHandle> ToolExecutor::offered(const ToolAudience& audience) const
{
  std::vector<tools::ToolHandle> out;
  for (const auto& tool : registry_.all())
    if (tool_access::holds(audience, tool->spec))
      out.push_back(tool);
  return out;
}

tools::ToolResult ToolExecutor::inactive(const tools::ToolCall& call, const tools::ToolDescriptor& tool,
                                         const ToolAudience& audience) const
{
  tools::ToolResult result = refused(
      call, "module_inactive", moduleOfferText({.audience = audience, .module = tool.spec.module, .lang = call.context.lang}));
  result.data["module"] = tool.spec.module;
  result.data["facts"] = moduleOfferFacts({.audience = audience, .module = tool.spec.module, .lang = call.context.lang});
  grounding_.remember({.call = call, .spec = tool.spec, .audience = audience}, result);
  if (ledger_ && !isAppTool(tool.spec.name) && !tool.spec.annotations.destructive)
    ledger_->offered({.userId = call.context.userId,
                      .role = audience.role,
                      .module = tool.spec.module,
                      .tool = tool.spec.name,
                      .arguments = call.arguments,
                      .lang = call.context.lang,
                      .utterance = call.context.utterance,
                      .sessionId = call.context.sessionId});
  return result;
}

tools::ToolResult ToolExecutor::execute(const tools::ToolCall& call, const ToolAudience& audience) const
{
  const auto tool = registry_.find(call.name);
  if (!tool)
    return refused(call, "unknown_tool", "unknown tool: " + call.name);
  const argus::mcp::ToolSpec& spec = tool->spec;
  if (!tool_access::holds(audience, spec))
    return refused(call, "forbidden", "permission denied for tool: " + call.name);

  tools::ToolCall prepared = call;
  prepared.context.role = audience.role;
  dropUndeclared(spec.inputSchema, prepared.arguments);
  time_arguments::normalize({.call = prepared, .spec = spec, .now = prepared.context.now.value_or(static_cast<int64_t>(std::time(nullptr)))});
  if (const auto invalid = argus::mcp::schema::violation(spec.inputSchema, prepared.arguments))
    return refused(call, "invalid_arguments", *invalid);
  if (tool_access::moduleInactive(audience, spec))
    return inactive(prepared, *tool, audience);
  const GroundingInput grounding{.call = prepared, .spec = spec, .audience = audience};
  if (auto grounded = grounding_.refuse(grounding))
    return *grounded;

  if (!tool->handler)
    return refused(call, "unavailable", std::string(toolUnreachable(call.context.lang)));
  tools::ToolResult result = tool->handler(prepared);
  result.tool = call.name;
  grounding_.remember(grounding, result);
  if (ledger_ && result.ok && spec.name == kEnableTool && prepared.arguments["module"].isString())
    ledger_->accepted({.userId = prepared.context.userId, .module = prepared.arguments["module"].asString()});
  return result;
}
