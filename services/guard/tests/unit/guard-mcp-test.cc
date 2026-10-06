#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/module-gate.hxx>
#include <feature/mcp/services/guard-tools.hxx>
#include <mcp/json-rpc.hxx>

#include <trantor/net/EventLoopThread.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace argus::mcp;

template <class T>
T must(std::optional<T> value)
{
  REQUIRE(value.has_value());
  return std::move(value).value_or(T{});
}

std::vector<EnvironmentChoice> defaultPlaces()
{
  return {{.id = 1, .name = "Casa"}, {.id = 2, .name = "Casa de campo"}, {.id = 3, .name = "Restaurante"}};
}

struct Harness
{
  trantor::EventLoopThread thread;
  std::shared_ptr<McpServer> server;
  int reads{0};

  explicit Harness(std::vector<EnvironmentChoice> places = defaultPlaces())
  {
    thread.run();
    trantor::EventLoop* loop = thread.getLoop();
    server = guardToolServer({.catalog = [this, places = std::move(places)]() -> drogon::Task<std::vector<EnvironmentChoice>> {
                                ++reads;
                                co_return places;
                              },
                              .loop = [loop] { return loop; }});
    moduleGate().reset();
  }

  ToolOutcome call(const std::string& role, const std::string& lang, Json::Value arguments)
  {
    Json::Value info(Json::objectValue);
    Json::Value params(Json::objectValue);
    params["name"] = "app.set_guard_mode";
    params["arguments"] = std::move(arguments);
    params["_meta"] = requestMeta(info);
    params["_meta"]["argus/context"] = toJson(CallerContext{
        .userId = 4, .role = role, .lang = lang, .sessionId = "s", .utterance = "pon la vigilancia", .decided = false});
    const auto response = must(parseResponse(server->handleBlocking(
        requestFrame({.hasId = true, .id = Json::Value(1), .method = "tools/call", .params = params}))));
    REQUIRE(response.result.has_value());
    return must(toolOutcomeFrom(response.result.value_or(Json::Value())));
  }
};

struct ModeArguments
{
  std::string mode;
  std::string environment;
};

Json::Value inPlace(const ModeArguments& input)
{
  Json::Value arguments(Json::objectValue);
  arguments["mode"] = input.mode;
  if (!input.environment.empty())
    arguments["environment"] = input.environment;
  return arguments;
}

Json::Value args(const std::string& mode)
{
  return inPlace({.mode = mode, .environment = ""});
}
}

TEST_CASE("the tool is the guard's, in the surveillance module, behind the capability that sets the mode")
{
  Harness harness;
  const auto* spec = harness.server->find("app.set_guard_mode");
  REQUIRE(spec != nullptr);
  CHECK(spec->module == "surveillance");
  CHECK(spec->capability == "guard.mode.set");
  CHECK(spec->inputSchema["required"][0].asString() == "mode");
}

TEST_CASE("a mode without a place changes every place and says so in the user's words")
{
  Harness harness;
  const auto outcome = harness.call("resident", "es", args("night"));
  CHECK_FALSE(outcome.isError);
  CHECK(outcome.text == "La app puso la vigilancia en modo noche.");
  const auto action = must(outcome.appAction);
  CHECK(action.name == "app.set_guard_mode");
  CHECK(action.arguments["mode"].asString() == "night");
  CHECK_FALSE(action.arguments.isMember("environment"));
  CHECK(harness.reads == 0);
}

TEST_CASE("the modes keep the labels the app always spoke")
{
  Harness harness;
  CHECK(harness.call("owner", "es", args("home")).text == "La app puso la vigilancia en modo en casa.");
  CHECK(harness.call("owner", "es", args("away")).text == "La app puso la vigilancia en modo fuera de casa.");
  CHECK(harness.call("owner", "es", args("armed")).text == "La app puso la vigilancia en modo máxima alerta.");
  CHECK(harness.call("owner", "en", args("away")).text == "The app set the guard mode to away.");
}

TEST_CASE("a named place is resolved and the app is told its id")
{
  Harness harness;
  const auto outcome = harness.call("owner", "es", inPlace({.mode = "armed", .environment = "restaurante"}));
  CHECK(outcome.text == "La app puso la vigilancia de Restaurante en modo máxima alerta.");
  const auto action = must(outcome.appAction);
  CHECK(action.arguments["environment"].asString() == "Restaurante");
  CHECK(action.arguments["environmentId"].asInt64() == 3);

  const auto english = harness.call("owner", "en", inPlace({.mode = "night", .environment = "casa de campo"}));
  CHECK(english.text == "The app set the guard mode of Casa de campo to night.");
  CHECK(must(english.appAction).arguments["environmentId"].asInt64() == 2);
}

TEST_CASE("an exact name wins over a longer name that holds it")
{
  Harness harness;
  const auto outcome = harness.call("owner", "es", inPlace({.mode = "home", .environment = "casa"}));
  CHECK_FALSE(outcome.isError);
  CHECK(must(outcome.appAction).arguments["environmentId"].asInt64() == 1);
}

TEST_CASE("a place that does not exist is refused with the places that do, and nothing is changed")
{
  Harness harness;
  const auto outcome = harness.call("resident", "es", inPlace({.mode = "night", .environment = "oficina"}));
  CHECK(outcome.isError);
  CHECK(outcome.text == "No encuentro un lugar llamado oficina. Los lugares son: Casa, Casa de campo, Restaurante.");
  CHECK(outcome.structured["code"].asString() == "unknown_environment");
  CHECK_FALSE(outcome.appAction.has_value());
}

TEST_CASE("a word two places hold asks which place")
{
  Harness twin({{.id = 1, .name = "Casa norte"}, {.id = 2, .name = "Casa sur"}});
  const auto asked = twin.call("resident", "en", inPlace({.mode = "night", .environment = "casa"}));
  CHECK(asked.isError);
  CHECK(asked.text == "Which place do you mean? Casa norte, Casa sur.");
  CHECK(asked.structured["code"].asString() == "ambiguous_environment");
}

TEST_CASE("only the roles that may set the mode are served")
{
  Harness harness;
  for (const char* role : {"owner", "resident"})
    CHECK_FALSE(harness.call(role, "es", args("night")).isError);
  for (const char* role : {"guard", "guest", "unknown"}) {
    const auto refused = harness.call(role, "es", args("night"));
    CHECK(refused.isError);
    CHECK(refused.structured["code"].asString() == "forbidden");
    CHECK_FALSE(refused.appAction.has_value());
  }
}

TEST_CASE("a surveillance module that is off refuses the tool")
{
  Harness harness;
  moduleGate().apply({{.id = "surveillance", .enabled = false}});
  const auto refused = harness.call("owner", "es", args("night"));
  CHECK(refused.isError);
  CHECK(refused.structured["code"].asString() == "module_inactive");
  moduleGate().reset();
}

TEST_CASE("a mode the schema does not know never reaches the catalog")
{
  Harness harness;
  const auto outcome = harness.call("owner", "es", inPlace({.mode = "party", .environment = "casa"}));
  CHECK(outcome.isError);
  CHECK(outcome.text == "argument 'mode' has an invalid value");
  CHECK(harness.reads == 0);
}
