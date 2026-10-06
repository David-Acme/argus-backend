#pragma once

#include <mcp/schema.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <functional>
#include <string>
#include <utility>

namespace tool_stubs
{

struct StubTool
{
  std::string name;
  std::string capability;
  std::function<tools::ToolResult(const tools::ToolCall&)> handler;
  std::string module{"core"};
  Json::Value schema{argus::mcp::schema::object({{.name = "text", .schema = argus::mcp::schema::text(), .required = false}})};
  bool destructive{false};
  argus::mcp::ToolPolicy policy{};
};

inline tools::ToolDescriptor stub(StubTool input)
{
  return {.spec = {.name = std::move(input.name),
                   .title = "",
                   .description = "probe",
                   .inputSchema = std::move(input.schema),
                   .annotations = {.destructive = input.destructive},
                   .module = std::move(input.module),
                   .capability = std::move(input.capability),
                   .policy = std::move(input.policy)},
          .handler = std::move(input.handler)};
}

inline tools::ToolResult okResult(std::string output)
{
  tools::ToolResult result;
  result.ok = true;
  result.output = std::move(output);
  return result;
}

struct AppStub
{
  std::string name;
  std::string capability;
  std::string module;
};

inline tools::ToolDescriptor appAction(const AppStub& app)
{
  const std::string name = app.name;
  return stub({.name = app.name,
               .capability = app.capability,
               .handler = [name](const tools::ToolCall& call) {
                 if (call.context.emitAction)
                   call.context.emitAction(name, call.arguments);
                 return okResult("done " + name);
               },
               .module = app.module,
               .schema = argus::mcp::schema::object(
                   {{.name = "mode", .schema = argus::mcp::schema::choice({"home", "night", "away", "armed"}), .required = false},
                    {.name = "camera", .schema = argus::mcp::schema::text(), .required = false},
                    {.name = "screen", .schema = argus::mcp::schema::text(), .required = false},
                    {.name = "environment", .schema = argus::mcp::schema::text(), .required = false}})});
}

}
