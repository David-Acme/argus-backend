#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/llm/lfm-adapter.hxx>

#include <string>
#include <vector>

TEST_CASE("the sentinel block yields a pythonic call with its arguments")
{
  const auto calls = LfmAdapter::parseToolCalls(
      "<|tool_call_start|>[memory.remember(subject=\"perro\", "
      "predicate=\"come\", value=\"7\")]<|tool_call_end|>Listo.");

  REQUIRE(calls.size() == 1);
  CHECK(calls[0].name == "memory.remember");
  CHECK(calls[0].arguments["subject"].asString() == "perro");
  CHECK(calls[0].arguments["predicate"].asString() == "come");
  CHECK(calls[0].arguments["value"].asString() == "7");
}

TEST_CASE("keys arriving with quotes and separators land under their own name")
{
  const auto calls = LfmAdapter::parseToolCalls(
      "<|tool_call_start|>[memory.remember(subject=\"hermana\", "
      "text=\"viene los domingos\", predicate\"=\"schedule\")]"
      "<|tool_call_end|>");

  REQUIRE(calls.size() == 1);
  CHECK(calls[0].arguments.isMember("predicate"));
  CHECK(calls[0].arguments["predicate"].asString() == "schedule");
  CHECK_FALSE(calls[0].arguments.isMember("\", predicate\""));
}

TEST_CASE("bare arguments keep the schema's types")
{
  const auto calls =
      LfmAdapter::parseToolCalls("[memory.forget(fact_id=42, confidence=0.9, "
                                 "scope=house)]");

  REQUIRE(calls.size() == 1);
  CHECK(calls[0].arguments["fact_id"].isIntegral());
  CHECK(calls[0].arguments["fact_id"].asInt64() == 42);
  CHECK(calls[0].arguments["confidence"].isDouble());
  CHECK(calls[0].arguments["confidence"].asDouble() == doctest::Approx(0.9));
  CHECK(calls[0].arguments["scope"].isString());
}

TEST_CASE("a JSON object call parses and prose yields nothing")
{
  const auto json = LfmAdapter::parseToolCalls(
      "{\"name\": \"memory.recall\", \"arguments\": {\"query\": \"perro\"}}");
  REQUIRE(json.size() == 1);
  CHECK(json[0].name == "memory.recall");
  CHECK(json[0].arguments["query"].asString() == "perro");

  CHECK(LfmAdapter::parseToolCalls("Claro, lo anoto.").empty());
}

TEST_CASE("a streaming hop holds only what can still open a tool call")
{
  CHECK(LfmAdapter::mayOpenToolCall(""));
  CHECK(LfmAdapter::mayOpenToolCall("<|"));
  CHECK(LfmAdapter::mayOpenToolCall("<|tool_call_start|>[memory"));
  CHECK(LfmAdapter::mayOpenToolCall("  \n[memory.remember("));
  CHECK(LfmAdapter::mayOpenToolCall("{\"name\""));

  CHECK_FALSE(LfmAdapter::mayOpenToolCall("C"));
  CHECK_FALSE(LfmAdapter::mayOpenToolCall("Claro, lo anoto."));
  CHECK_FALSE(LfmAdapter::mayOpenToolCall("<x"));
}

TEST_CASE("declarations omit the required list when no argument is required")
{
  const tools::ToolDescriptor remember{
      .name = "memory.remember",
      .description = "stores a fact",
      .arguments = {{"text", "string", false, {}, ""},
                    {"type", "enum", false, {"schedule", "attribute"}, ""}},
      .accessTable = TableName::Memory,
      .accessPermission = RolePermission::Create,
      .handler = nullptr};
  const tools::ToolDescriptor recall{
      .name = "memory.recall",
      .description = "reads a fact",
      .arguments = {{"query", "string", true, {}, ""}},
      .accessTable = TableName::Memory,
      .accessPermission = RolePermission::Read,
      .handler = nullptr};

  const std::string open =
      LfmAdapter::buildToolDeclarations({&remember});
  CHECK(open.find("\"required\"") == std::string::npos);
  CHECK(open.find("\"enum\":[\"schedule\",\"attribute\"]") !=
        std::string::npos);

  const std::string closed = LfmAdapter::buildToolDeclarations({&recall});
  CHECK(closed.find("\"required\":[\"query\"]") != std::string::npos);
}

TEST_CASE("the registry resolves a tool whose name arrives capitalized")
{
  tools::ToolDescriptor remember{.name = "memory.remember",
                                 .description = "stores a fact",
                                 .arguments = {},
                                 .accessTable = TableName::Memory,
                                 .accessPermission = RolePermission::Create,
                                 .handler = nullptr};
  auto& registry = ToolRegistry::instance();
  registry.registerTool(remember);

  CHECK(registry.find("Memory.remember") != nullptr);
  CHECK(registry.find("MEMORY.REMEMBER") != nullptr);
  CHECK(registry.find("memory.forget") == nullptr);
}
