#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test-support.hxx"

#include <mcp/schema.hxx>
#include <mcp/tool.hxx>

#include <string>

using namespace argus::mcp;
using test_support::must;

namespace
{
ToolSpec sample()
{
  return {.name = "calendar.create_event",
          .title = "Create an event",
          .description = "Adds an event to the agenda",
          .inputSchema = schema::object({{.name = "title", .schema = schema::text(), .required = true}}),
          .annotations = {.readOnly = false, .destructive = true, .idempotent = false, .openWorld = false},
          .module = "productivity",
          .capability = "agenda.write"};
}
}

TEST_CASE("tool names follow the protocol's character set and length")
{
  CHECK(validToolName("memory.remember"));
  CHECK(validToolName("DATA_EXPORT-v2"));
  CHECK_FALSE(validToolName(""));
  CHECK_FALSE(validToolName("has space"));
  CHECK_FALSE(validToolName("comma,name"));
  CHECK_FALSE(validToolName("ñandú"));
  CHECK_FALSE(validToolName(std::string(129, 'a')));
  CHECK(validToolName(std::string(128, 'a')));
}

TEST_CASE("a tool spec round trips with its module and capability in _meta")
{
  const Json::Value json = toJson(sample());
  CHECK(json["_meta"]["argus/module"].asString() == "productivity");
  CHECK(json["_meta"]["argus/capability"].asString() == "agenda.write");
  CHECK(json["annotations"]["destructiveHint"].asBool());
  const auto back = must(toolSpecFrom(json));
  CHECK(back.name == "calendar.create_event");
  CHECK(back.title == "Create an event");
  CHECK(back.module == "productivity");
  CHECK(back.capability == "agenda.write");
  CHECK(back.annotations.destructive);
  CHECK_FALSE(back.annotations.readOnly);
  CHECK(back.inputSchema["required"][0].asString() == "title");
}

TEST_CASE("a tool's policy lines travel in _meta in both languages and are omitted when empty")
{
  ToolSpec spec = sample();
  spec.policy = {.spanish = "Para agendar usa esta herramienta.", .english = "To schedule, use this tool."};
  const Json::Value json = toJson(spec);
  CHECK(json["_meta"]["argus/policy"]["es"].asString() == "Para agendar usa esta herramienta.");
  CHECK(json["_meta"]["argus/policy"]["en"].asString() == "To schedule, use this tool.");
  const auto back = must(toolSpecFrom(json));
  CHECK(back.policy.spanish == "Para agendar usa esta herramienta.");
  CHECK(back.policy.english == "To schedule, use this tool.");

  const auto bare = must(toolSpecFrom(toJson(sample())));
  CHECK(bare.policy.spanish.empty());
  CHECK(bare.policy.english.empty());
  CHECK_FALSE(toJson(sample())["_meta"].isMember("argus/policy"));
}

TEST_CASE("a spec without a module or capability omits _meta")
{
  ToolSpec spec = sample();
  spec.module.clear();
  spec.capability.clear();
  CHECK_FALSE(toJson(spec).isMember("_meta"));
  const auto back = must(toolSpecFrom(toJson(spec)));
  CHECK(back.module.empty());
  CHECK(back.capability.empty());
}

TEST_CASE("a listed entry that is not a tool is not a spec")
{
  CHECK_FALSE(toolSpecFrom(Json::Value("x")).has_value());
  Json::Value broken = toJson(sample());
  broken["name"] = "bad name";
  CHECK_FALSE(toolSpecFrom(broken).has_value());
  Json::Value schemaless = toJson(sample());
  schemaless.removeMember("inputSchema");
  CHECK_FALSE(toolSpecFrom(schemaless).has_value());
}

TEST_CASE("the caller context round trips and tolerates garbage")
{
  const CallerContext caller{.userId = 42, .role = "resident", .lang = "es", .sessionId = "s1", .utterance = "hola", .decided = true};
  const CallerContext back = callerContextFrom(toJson(caller));
  CHECK(back.userId == 42);
  CHECK(back.role == "resident");
  CHECK(back.lang == "es");
  CHECK(back.sessionId == "s1");
  CHECK(back.utterance == "hola");
  CHECK(back.decided);
  const CallerContext empty = callerContextFrom(Json::Value("nope"));
  CHECK(empty.userId == 0);
  CHECK(empty.role.empty());
}

TEST_CASE("an outcome keeps its text, its structure, its error flag and its app action")
{
  ToolOutcome outcome;
  outcome.text = "La app está mostrando la cámara garaje.";
  outcome.structured["cameraId"] = 3;
  outcome.appAction = AppAction{.name = "app.show_camera", .arguments = Json::Value(Json::objectValue)};
  outcome.appAction->arguments["camera"] = "garaje";
  const auto back = must(toolOutcomeFrom(toJson(outcome)));
  CHECK(back.text == outcome.text);
  CHECK_FALSE(back.isError);
  CHECK(back.structured["cameraId"].asInt() == 3);
  const auto action = must(back.appAction);
  CHECK(action.name == "app.show_camera");
  CHECK(action.arguments["camera"].asString() == "garaje");
}

TEST_CASE("an error outcome says so and carries no app action")
{
  ToolOutcome outcome;
  outcome.isError = true;
  outcome.text = "permission denied for tool: x";
  const auto back = must(toolOutcomeFrom(toJson(outcome)));
  CHECK(back.isError);
  CHECK_FALSE(back.appAction.has_value());
}

TEST_CASE("a result of another type or without content is not an outcome")
{
  Json::Value pending(Json::objectValue);
  pending["resultType"] = "input_required";
  pending["content"] = Json::Value(Json::arrayValue);
  CHECK_FALSE(toolOutcomeFrom(pending).has_value());
  CHECK_FALSE(toolOutcomeFrom(Json::Value(Json::objectValue)).has_value());
}

TEST_CASE("several text blocks join in order and other blocks are skipped")
{
  Json::Value result(Json::objectValue);
  Json::Value content(Json::arrayValue);
  Json::Value first(Json::objectValue);
  first["type"] = "text";
  first["text"] = "uno";
  Json::Value image(Json::objectValue);
  image["type"] = "image";
  Json::Value second(Json::objectValue);
  second["type"] = "text";
  second["text"] = "dos";
  content.append(first);
  content.append(image);
  content.append(second);
  result["content"] = content;
  const auto outcome = must(toolOutcomeFrom(result));
  CHECK(outcome.text == "uno\ndos");
}

TEST_CASE("the request meta names the version and declares no client capability")
{
  Json::Value info(Json::objectValue);
  info["name"] = "argus-llm";
  const Json::Value meta = requestMeta(info);
  CHECK(meta["io.modelcontextprotocol/protocolVersion"].asString() == "2026-07-28");
  CHECK(meta["io.modelcontextprotocol/clientCapabilities"].isObject());
  CHECK(meta["io.modelcontextprotocol/clientInfo"]["name"].asString() == "argus-llm");
}

TEST_CASE("a failure is an error outcome that carries its code and no action")
{
  const auto failure = toolFailure({.text = "No encuentro esa cámara", .code = "unknown_camera"});
  CHECK(failure.isError);
  CHECK(failure.text == "No encuentro esa cámara");
  CHECK(failure.structured["code"].asString() == "unknown_camera");
  CHECK_FALSE(failure.appAction.has_value());
  const auto back = must(toolOutcomeFrom(toJson(failure)));
  CHECK(back.isError);
  CHECK(back.structured["code"].asString() == "unknown_camera");
}
