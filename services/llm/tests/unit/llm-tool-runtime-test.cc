#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/contracts/tool-contracts.hxx>
#include <shared/services/memory/memory-tool-descriptors.hxx>
#include <shared/services/tools/tool-executor.hxx>
#include <shared/services/tools/tool-registry.hxx>
#include <shared/services/tools/tool-validator.hxx>

#include <json/value.h>
#include <optional>
#include <string>
#include <vector>

namespace
{

// A descriptor whose handler records that it ran, so a refusal can be told
// apart from a dispatch. Its schema is all-optional, memory's own convention:
// a fired call must reach the gate, never die on the way there.
struct Probe
{
  bool ran = false;
  std::string seen;
};

tools::ToolDescriptor probeDescriptor(const std::string& name, Probe& probe,
                                      TableName table, RolePermission perm)
{
  return {.name = name,
          .description = "probe",
          .arguments = {{.name = "query",
                         .type = "string",
                         .required = false,
                         .enumValues = {},
                         .description = ""}},
          .accessTable = table,
          .accessPermission = perm,
          .handler = [&probe](const tools::ToolCall& call) {
            probe.ran = true;
            probe.seen = call.arguments.get("query", "").asString();
            tools::ToolResult result;
            result.ok = true;
            result.output = "ran " + call.name;
            // Deliberately not the call's name: the executor stamps that.
            result.tool = "probe";
            result.data["seen"] = probe.seen;
            return result;
          }};
}

tools::ToolCall callFor(const std::string& name)
{
  tools::ToolCall call;
  call.name = name;
  call.arguments = Json::Value(Json::objectValue);
  call.context.userId = 7;
  call.context.utterance = "recuerdame algo";
  // The arguments memory's tools require, so a call reaches the gate.
  if (name == "memory.recall")
    call.arguments["query"] = "dentista";
  if (name == "procedure.run")
    call.arguments["goal"] = "apagar la luz";
  if (name == "memory.forget")
    call.arguments["fact_id"] = 42;
  return call;
}

// The same call with nothing filled in: what a model emits when it fires a
// tool with no arguments.
tools::ToolCall bareCall(const std::string& name)
{
  tools::ToolCall call = callFor(name);
  call.arguments = Json::Value(Json::objectValue);
  return call;
}

// The descriptors memory really ships, not a copy of them.
const tools::ToolDescriptor* declaredByMemory(const std::string& name)
{
  static const std::vector<tools::ToolDescriptor> declared =
      memoryToolDescriptors();
  for (const auto& descriptor : declared)
    if (descriptor.name == name)
      return &descriptor;
  return nullptr;
}

// Memory's declarations, registered the way main.cc registers them; the
// handlers stay null, so a call the gate let through would abort here instead
// of answering — which is the point of the refusal cases below.
void registerMemoryTools(ToolRegistry& registry)
{
  for (auto descriptor : memoryToolDescriptors())
    registry.registerTool(std::move(descriptor));
}

} // namespace

TEST_CASE("the executor dispatches a registered tool and refuses a name it "
          "does not hold")
{
  Probe probe;
  Probe never;
  ToolRegistry registry;
  registry.registerTool(
      probeDescriptor("probe.remember", probe, TableName::Memory,
                      RolePermission::Create));
  // A near miss of the registered name: resolution is a lookup, not a prefix.
  registry.registerTool(
      probeDescriptor("probe.forget", never, TableName::Memory,
                      RolePermission::Delete));
  const ToolExecutor executor(registry);

  auto call = callFor("probe.remember");
  call.arguments["query"] = "dentista";
  const auto dispatched = executor.execute(call, UserRole::Resident);
  INFO("dispatch output: " << dispatched.output);
  CHECK(dispatched.ok);
  CHECK(probe.ran);
  CHECK(probe.seen == "dentista");
  CHECK_FALSE(never.ran);

  const auto unknown =
      executor.execute(callFor("probe.forgotten"), UserRole::Resident);
  CHECK_FALSE(unknown.ok);
  CHECK(unknown.output == "unknown tool: probe.forgotten");
  CHECK_FALSE(never.ran);
}

TEST_CASE("a call the schema rejects never reaches the handler")
{
  ToolRegistry registry;
  registerMemoryTools(registry);
  const ToolExecutor executor(registry);

  // A required argument is missing.
  const auto missing =
      executor.execute(bareCall("memory.recall"), UserRole::Resident);
  CHECK_FALSE(missing.ok);
  CHECK(missing.output == "missing required argument 'query'");

  // An enum argument carries a value the schema does not list.
  auto wrongEnum = callFor("memory.remember");
  wrongEnum.arguments["type"] = "inventado";
  const auto enumRefused = executor.execute(wrongEnum, UserRole::Resident);
  CHECK_FALSE(enumRefused.ok);
  CHECK(enumRefused.output == "argument 'type' has an invalid value");

  // A number argument that is not a number.
  auto wrongType = callFor("memory.forget");
  wrongType.arguments["fact_id"] = "cuarenta y dos";
  const auto typeRefused = executor.execute(wrongType, UserRole::Resident);
  CHECK_FALSE(typeRefused.ok);
  CHECK(typeRefused.output == "argument 'fact_id' must be a number");

  // Arguments that are not an object at all.
  auto notObject = callFor("memory.remember");
  notObject.arguments = Json::Value("guardalo");
  const auto shapeRefused = executor.execute(notObject, UserRole::Resident);
  CHECK_FALSE(shapeRefused.ok);
  CHECK(shapeRefused.output == "arguments must be a JSON object");
}

TEST_CASE("a role the table does not grant never reaches the handler")
{
  Probe probe;
  ToolRegistry registry;
  registry.registerTool(
      probeDescriptor("probe.remember", probe, TableName::Memory,
                      RolePermission::Create));
  const ToolExecutor executor(registry);

  // A guest holds no Memory row, so the gate stops the call; the same call
  // clears it for a resident, which is what makes the refusal the gate's and
  // not the schema's.
  const auto refused =
      executor.execute(callFor("probe.remember"), UserRole::Guest);
  CHECK_FALSE(refused.ok);
  CHECK(refused.output == "permission denied for tool: probe.remember");
  CHECK_FALSE(probe.ran);

  const auto granted =
      executor.execute(callFor("probe.remember"), UserRole::Resident);
  CHECK(granted.ok);
  CHECK(probe.ran);
}

TEST_CASE("a dispatch comes back under the tool that was called")
{
  Probe probe;
  ToolRegistry registry;
  registry.registerTool(
      probeDescriptor("probe.remember", probe, TableName::Memory,
                      RolePermission::Create));
  const ToolExecutor executor(registry);

  auto call = callFor("probe.remember");
  call.arguments["query"] = "el dentista";
  const auto result = executor.execute(call, UserRole::Resident);

  REQUIRE(result.ok);
  CHECK(result.tool == "probe.remember");
  CHECK(result.output == "ran probe.remember");
  CHECK(result.data["seen"].asString() == "el dentista");
}

TEST_CASE("the memory descriptors declare the access the gate reads")
{
  const struct
  {
    const char* name;
    TableName table;
    RolePermission permission;
  } declared[] = {
      {"memory.remember", TableName::Memory, RolePermission::Create},
      {"memory.remind", TableName::Memory, RolePermission::Create},
      {"memory.recall", TableName::Memory, RolePermission::Read},
      {"procedure.run", TableName::Memory, RolePermission::Read},
      {"memory.forget", TableName::Memory, RolePermission::Delete}};

  for (const auto& expected : declared) {
    const auto* descriptor = declaredByMemory(expected.name);
    REQUIRE(descriptor != nullptr);
    CHECK(descriptor->accessTable == expected.table);
    CHECK(descriptor->accessPermission == expected.permission);
  }
}

TEST_CASE("the memory descriptors declare their required arguments")
{
  const struct
  {
    const char* name;
    const char* argument;
  } required[] = {{"memory.recall", "query"},
                  {"procedure.run", "goal"},
                  {"memory.forget", "fact_id"}};

  for (const auto& expected : required) {
    const auto* descriptor = declaredByMemory(expected.name);
    REQUIRE(descriptor != nullptr);
    const auto error = tools::validateArguments(*descriptor,
                                               bareCall(expected.name));
    CHECK(error == std::optional<std::string>{
                       std::string("missing required argument '") +
                       expected.argument + "'"});
    CHECK_FALSE(tools::validateArguments(*descriptor, callFor(expected.name))
                    .has_value());
  }

  // remember and remind take everything optionally on purpose: a fired call
  // must always reach the handler.
  for (const char* name : {"memory.remember", "memory.remind"}) {
    const auto* descriptor = declaredByMemory(name);
    REQUIRE(descriptor != nullptr);
    CHECK_FALSE(tools::validateArguments(*descriptor, bareCall(name))
                    .has_value());
  }
}

TEST_CASE("the gate refuses every memory tool for a role with no Memory row")
{
  ToolRegistry registry;
  registerMemoryTools(registry);
  const ToolExecutor executor(registry);

  for (const char* name : {"memory.remember", "memory.remind", "memory.recall",
                           "procedure.run", "memory.forget"}) {
    const auto refused = executor.execute(callFor(name), UserRole::Guest);
    INFO("refused " << name << ": " << refused.output);
    CHECK_FALSE(refused.ok);
    CHECK(refused.output == std::string("permission denied for tool: ") + name);
  }
}
