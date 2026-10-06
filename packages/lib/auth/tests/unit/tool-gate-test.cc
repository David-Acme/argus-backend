#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/module-gate.hxx>
#include <auth/tool-gate.hxx>

#include <string>

namespace
{
argus::mcp::ToolSpec spec(const std::string& capability, const std::string& module)
{
  return {.name = "calendar.create_event",
          .title = "",
          .description = "",
          .inputSchema = Json::Value(Json::objectValue),
          .annotations = {},
          .module = module,
          .capability = capability};
}

argus::mcp::ToolInvocation by(const std::string& role)
{
  return {.name = "calendar.create_event",
          .arguments = Json::Value(Json::objectValue),
          .caller = {.userId = 3, .role = role, .lang = "es", .sessionId = "", .utterance = "", .decided = false}};
}

struct ModulesOff
{
  ModulesOff()
  {
    moduleGate().reset();
    moduleGate().apply({{.id = "productivity", .enabled = false}, {.id = "surveillance", .enabled = true}});
  }
  ~ModulesOff() { moduleGate().reset(); }
};
}

TEST_CASE("a role that holds the capability and whose module is on passes")
{
  moduleGate().reset();
  const auto gate = tool_gate::capabilities();
  CHECK_FALSE(gate(by("resident"), spec("agenda.write", "productivity")).has_value());
  CHECK_FALSE(gate(by("owner"), spec("agenda.write", "productivity")).has_value());
}

TEST_CASE("a role that does not hold the capability is forbidden whatever the module says")
{
  moduleGate().reset();
  const auto gate = tool_gate::capabilities();
  const auto guest = gate(by("guest"), spec("agenda.write", "productivity"));
  REQUIRE(guest.has_value());
  CHECK(guest.value_or(argus::mcp::Refusal{}).code == "forbidden");
  CHECK(guest.value_or(argus::mcp::Refusal{}).message == "permission denied for tool: calendar.create_event");
}

TEST_CASE("an unknown role, an unnamed role and a tool with no capability or an invented one hold nothing")
{
  moduleGate().reset();
  const auto gate = tool_gate::capabilities();
  CHECK(gate(by("unknown"), spec("agenda.write", "productivity")).has_value());
  CHECK(gate(by(""), spec("agenda.write", "productivity")).has_value());
  CHECK(gate(by("owner"), spec("", "productivity")).has_value());
  CHECK(gate(by("owner"), spec("invented.capability", "productivity")).has_value());
}

TEST_CASE("a module that is off refuses its tools with a code the owner of the tool can tell apart")
{
  const ModulesOff off;
  const auto gate = tool_gate::capabilities();
  const auto refused = gate(by("resident"), spec("agenda.write", "productivity"));
  REQUIRE(refused.has_value());
  CHECK(refused.value_or(argus::mcp::Refusal{}).code == "module_inactive");
  CHECK_FALSE(gate(by("resident"), spec("camera.view", "surveillance")).has_value());
}
