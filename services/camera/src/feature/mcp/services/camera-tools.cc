#include "camera-tools.hxx"

#include <auth/tool-gate.hxx>
#include <mcp/loop-tool.hxx>
#include <mcp/schema.hxx>
#include <text/name-match.hxx>

#include <algorithm>
#include <charconv>
#include <string_view>
#include <utility>

namespace
{
namespace schema = argus::mcp::schema;

constexpr std::string_view kLive = "live";
constexpr std::string_view kSnapshot = "snapshot";

std::string listed(const std::vector<CameraChoice>& cameras)
{
  std::string out;
  for (const auto& camera : cameras)
    out += (out.empty() ? "" : ", ") + camera.name;
  return out;
}

std::string viewOf(const Json::Value& arguments)
{
  const std::string view = arguments.get("view", std::string(kLive)).asString();
  return view == kSnapshot ? std::string(kSnapshot) : std::string(kLive);
}

argus::mcp::ToolOutcome shown(const argus::mcp::ToolInvocation& invocation, const CameraMatch& match)
{
  const bool english = invocation.caller.lang == "en";
  const bool snapshot = viewOf(invocation.arguments) == kSnapshot;
  const bool named = match.kind == CameraMatchKind::Exact;
  argus::mcp::ToolOutcome outcome;
  if (!named)
    outcome.text = snapshot ? (english ? "The app is showing a recent picture from the last notice."
                                       : "La app está mostrando una foto reciente de la cámara del último aviso.")
                            : (english ? "The app is showing the camera from the last notice."
                                       : "La app está mostrando la cámara del último aviso.");
  else if (snapshot)
    outcome.text = english ? "The app is showing a recent picture from the " + match.camera.name + " camera."
                           : "La app está mostrando una foto reciente de la cámara " + match.camera.name + ".";
  else
    outcome.text = english ? "The app is showing the " + match.camera.name + " camera."
                           : "La app está mostrando la cámara " + match.camera.name + ".";
  Json::Value arguments(Json::objectValue);
  arguments["camera"] = named ? match.camera.name : std::string();
  arguments["view"] = viewOf(invocation.arguments);
  if (named)
    arguments["cameraId"] = match.camera.id;
  outcome.structured = arguments;
  outcome.appAction = argus::mcp::AppAction{.name = "app.show_camera", .arguments = std::move(arguments)};
  return outcome;
}


drogon::Task<argus::mcp::ToolOutcome> showCamera(CameraCatalog catalog, argus::mcp::ToolInvocation invocation)
{
  const std::string asked = invocation.arguments.get("camera", "").asString();
  if (text_norm::folded(asked).empty())
    co_return shown(invocation, {});
  const auto cameras = co_await catalog();
  const CameraMatch match = matchCamera({.cameras = cameras, .text = asked});
  const bool english = invocation.caller.lang == "en";
  if (match.kind == CameraMatchKind::Exact)
    co_return shown(invocation, match);
  if (match.kind == CameraMatchKind::Ambiguous)
    co_return argus::mcp::toolFailure({.text = english ? "Which camera do you mean? " + listed(match.candidates) + "."
                              : "¿A cuál cámara te refieres? " + listed(match.candidates) + ".", .code = "ambiguous_camera"});
  if (cameras.empty())
    co_return argus::mcp::toolFailure({.text = english ? "There are no cameras yet." : "Todavía no hay cámaras.", .code = "no_cameras"});
  co_return argus::mcp::toolFailure({.text = english ? "I could not find a camera called " + asked + ". The cameras are: " + listed(cameras) + "."
                            : "No encuentro una cámara llamada " + asked + ". Las cámaras son: " + listed(cameras) + ".", .code = "unknown_camera"});
}
}

CameraMatch matchCamera(const CameraQuery& query)
{
  CameraMatch match;
  const std::string spoken = text_norm::folded(query.text);
  if (spoken.empty())
    return match;
  int64_t number = 0;
  const auto [end, error] = std::from_chars(spoken.data(), spoken.data() + spoken.size(), number);
  if (error == std::errc{} && end == spoken.data() + spoken.size()) {
    const auto byId = std::ranges::find(query.cameras, number, &CameraChoice::id);
    if (byId != query.cameras.end()) {
      match.kind = CameraMatchKind::Exact;
      match.camera = *byId;
      return match;
    }
  }
  std::vector<std::string> names;
  names.reserve(query.cameras.size());
  for (const auto& camera : query.cameras)
    names.push_back(camera.name);
  const auto found = text_norm::matchName(names, query.text);
  if (found.kind == text_norm::NameMatchKind::Exact) {
    match.kind = CameraMatchKind::Exact;
    match.camera = query.cameras.at(found.hits.front());
  }
  else if (found.kind == text_norm::NameMatchKind::Ambiguous) {
    match.kind = CameraMatchKind::Ambiguous;
    for (const size_t index : found.hits)
      match.candidates.push_back(query.cameras.at(index));
  }
  return match;
}

std::shared_ptr<argus::mcp::McpServer> cameraToolServer(const CameraToolsInput& input)
{
  auto server = std::make_shared<argus::mcp::McpServer>(
      argus::mcp::ServerIdentity{.name = "argus-camera", .version = "1", .instructions = ""});
  const CameraCatalog catalog = input.catalog;
  server->add({.name = "app.show_camera",
               .title = "",
               .description = "Muestra en la app del usuario la imagen en vivo de una cámara, o una foto reciente si "
                              "view es snapshot. El argumento camera es el nombre de la cámara; vacío muestra la del "
                              "último aviso",
               .inputSchema = schema::object(
                   {{.name = "camera", .schema = schema::text(), .required = false},
                    {.name = "view", .schema = schema::choice({std::string(kLive), std::string(kSnapshot)}), .required = false}}),
               .annotations = {.readOnly = true},
               .module = "surveillance",
               .capability = "camera.view"},
              argus::mcp::onLoop({.loop = input.loop, .handler = [catalog](const argus::mcp::ToolInvocation& invocation) {
                return showCamera(catalog, invocation);
              }}));
  server->setGate(tool_gate::capabilities());
  return server;
}
