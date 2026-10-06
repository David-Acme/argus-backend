#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/module-gate.hxx>
#include <feature/mcp/services/camera-tools.hxx>
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

std::vector<CameraChoice> defaultCameras()
{
  return {{.id = 1, .name = "Garaje"},
          {.id = 2, .name = "Patio trasero"},
          {.id = 3, .name = "Patio delantero"},
          {.id = 4, .name = "Cámara de la sala"}};
}

struct Harness
{
  trantor::EventLoopThread thread;
  std::shared_ptr<McpServer> server;

  explicit Harness(std::vector<CameraChoice> cameras = defaultCameras())
  {
    thread.run();
    trantor::EventLoop* loop = thread.getLoop();
    server = cameraToolServer({.catalog = [cameras = std::move(cameras)]() -> drogon::Task<std::vector<CameraChoice>> {
                                 co_return cameras;
                               },
                               .loop = [loop] { return loop; }});
    moduleGate().reset();
  }

  ToolOutcome call(const std::string& role, const std::string& lang, Json::Value arguments)
  {
    Json::Value info(Json::objectValue);
    Json::Value params(Json::objectValue);
    params["name"] = "app.show_camera";
    params["arguments"] = std::move(arguments);
    params["_meta"] = requestMeta(info);
    params["_meta"]["argus/context"] = toJson(CallerContext{
        .userId = 4, .role = role, .lang = lang, .sessionId = "s", .utterance = "muéstrame", .decided = false});
    const auto frame = server->handleBlocking(
        requestFrame({.hasId = true, .id = Json::Value(1), .method = "tools/call", .params = params}));
    const auto response = must(parseResponse(frame));
    REQUIRE(response.result.has_value());
    return must(toolOutcomeFrom(response.result.value_or(Json::Value())));
  }
};

Json::Value args(const std::string& camera, const std::string& view = "")
{
  Json::Value arguments(Json::objectValue);
  if (!camera.empty())
    arguments["camera"] = camera;
  if (!view.empty())
    arguments["view"] = view;
  return arguments;
}
}

TEST_CASE("a camera is matched by its whole name without caring for case or accents")
{
  const auto match = matchCamera({.cameras = defaultCameras(), .text = "CAMARA de la SALA"});
  CHECK(match.kind == CameraMatchKind::Exact);
  CHECK(match.camera.id == 4);
}

TEST_CASE("a camera is matched by its number")
{
  const auto match = matchCamera({.cameras = defaultCameras(), .text = "2"});
  CHECK(match.kind == CameraMatchKind::Exact);
  CHECK(match.camera.name == "Patio trasero");
  CHECK(matchCamera({.cameras = defaultCameras(), .text = "9"}).kind == CameraMatchKind::Missing);
}

TEST_CASE("a camera is matched by a word that only one name holds")
{
  const auto match = matchCamera({.cameras = defaultCameras(), .text = "el garaje"});
  CHECK(match.kind == CameraMatchKind::Missing);
  const auto word = matchCamera({.cameras = defaultCameras(), .text = "garaje"});
  CHECK(word.kind == CameraMatchKind::Exact);
  CHECK(word.camera.id == 1);
  CHECK(matchCamera({.cameras = defaultCameras(), .text = "trasero"}).camera.id == 2);
}

TEST_CASE("a word two names hold is ambiguous and lists both")
{
  const auto match = matchCamera({.cameras = defaultCameras(), .text = "patio"});
  CHECK(match.kind == CameraMatchKind::Ambiguous);
  REQUIRE(match.candidates.size() == 2);
  CHECK(match.candidates[0].id == 2);
  CHECK(match.candidates[1].id == 3);
}

TEST_CASE("nothing, or a name no camera has, is missing")
{
  CHECK(matchCamera({.cameras = defaultCameras(), .text = ""}).kind == CameraMatchKind::Missing);
  CHECK(matchCamera({.cameras = defaultCameras(), .text = "   "}).kind == CameraMatchKind::Missing);
  CHECK(matchCamera({.cameras = defaultCameras(), .text = "jardín"}).kind == CameraMatchKind::Missing);
  CHECK(matchCamera({.cameras = {}, .text = "garaje"}).kind == CameraMatchKind::Missing);
}

TEST_CASE("the tool carries its module, its capability and its two arguments")
{
  Harness harness;
  const auto* spec = harness.server->find("app.show_camera");
  REQUIRE(spec != nullptr);
  CHECK(spec->module == "surveillance");
  CHECK(spec->capability == "camera.view");
  CHECK(spec->inputSchema["properties"]["view"]["enum"].size() == 2);
}

TEST_CASE("a named camera is shown live and the app is told which one")
{
  Harness harness;
  const auto outcome = harness.call("resident", "es", args("garaje"));
  CHECK_FALSE(outcome.isError);
  CHECK(outcome.text == "La app está mostrando la cámara Garaje.");
  const auto action = must(outcome.appAction);
  CHECK(action.name == "app.show_camera");
  CHECK(action.arguments["camera"].asString() == "Garaje");
  CHECK(action.arguments["cameraId"].asInt64() == 1);
  CHECK(action.arguments["view"].asString() == "live");
}

TEST_CASE("a snapshot is asked for by the view argument and spoken in the user's language")
{
  Harness harness;
  const auto outcome = harness.call("owner", "en", args("garaje", "snapshot"));
  CHECK(outcome.text == "The app is showing a recent picture from the Garaje camera.");
  CHECK(must(outcome.appAction).arguments["view"].asString() == "snapshot");
}

TEST_CASE("no name shows the camera of the last notice and names none")
{
  Harness harness;
  const auto outcome = harness.call("resident", "es", args(""));
  CHECK(outcome.text == "La app está mostrando la cámara del último aviso.");
  const auto action = must(outcome.appAction);
  CHECK(action.arguments["camera"].asString().empty());
  CHECK_FALSE(action.arguments.isMember("cameraId"));
}

TEST_CASE("a camera that does not exist is refused with the cameras that do, and nothing is shown")
{
  Harness harness;
  const auto outcome = harness.call("resident", "es", args("jardín"));
  CHECK(outcome.isError);
  CHECK(outcome.text == "No encuentro una cámara llamada jardín. Las cámaras son: Garaje, Patio trasero, Patio delantero, Cámara de la sala.");
  CHECK(outcome.structured["code"].asString() == "unknown_camera");
  CHECK_FALSE(outcome.appAction.has_value());
}

TEST_CASE("an ambiguous name asks which camera, and an empty installation says there are none")
{
  Harness harness;
  const auto ambiguous = harness.call("resident", "en", args("patio"));
  CHECK(ambiguous.isError);
  CHECK(ambiguous.text == "Which camera do you mean? Patio trasero, Patio delantero.");
  CHECK(ambiguous.structured["code"].asString() == "ambiguous_camera");

  Harness empty({});
  const auto none = empty.call("resident", "es", args("garaje"));
  CHECK(none.isError);
  CHECK(none.text == "Todavía no hay cámaras.");
}

TEST_CASE("every role that may watch is served and an unknown role is not")
{
  Harness harness;
  for (const char* role : {"owner", "resident", "guard", "guest"})
    CHECK_FALSE(harness.call(role, "es", args("garaje")).isError);
  const auto refused = harness.call("unknown", "es", args("garaje"));
  CHECK(refused.isError);
  CHECK(refused.structured["code"].asString() == "forbidden");
  CHECK_FALSE(refused.appAction.has_value());
}

TEST_CASE("a surveillance module that is off refuses the tool")
{
  Harness harness;
  moduleGate().apply({{.id = "surveillance", .enabled = false}});
  const auto refused = harness.call("resident", "es", args("garaje"));
  CHECK(refused.isError);
  CHECK(refused.structured["code"].asString() == "module_inactive");
  moduleGate().reset();
}

TEST_CASE("a bad view is refused by the schema before the catalog is read")
{
  Harness harness;
  const auto outcome = harness.call("resident", "es", args("garaje", "panorama"));
  CHECK(outcome.isError);
  CHECK(outcome.text == "argument 'view' has an invalid value");
}
