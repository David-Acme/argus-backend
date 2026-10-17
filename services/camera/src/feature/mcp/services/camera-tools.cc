#include "camera-tools.hxx"

#include <auth/tool-gate.hxx>
#include <feature/camera-control/dtos/camera-settings-dto.hxx>
#include <feature/camera-control/services/camera-control-feature-service.hxx>
#include <shared/services/privacy-state/privacy-state.hxx>
#include <mcp/loop-tool.hxx>
#include <mcp/schema.hxx>
#include <text/name-match.hxx>

#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>
#include <utility>

namespace
{
namespace schema = argus::mcp::schema;

constexpr std::string_view kLive = "live";
constexpr std::string_view kSnapshot = "snapshot";
constexpr std::string_view kSetPrivacyTool = "camera.set_privacy";

argus::mcp::ToolOutcome privacyRefusal(const argus::mcp::ToolInvocation& invocation,
                                       const std::string& spanish,
                                       const std::string& english,
                                       std::string code)
{
  return argus::mcp::toolFailure(
      {.text = invocation.caller.lang == "en" ? english : spanish,
       .code = std::move(code)});
}

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
  const bool masked = named && match.camera.privacyObeyed;
  argus::mcp::ToolOutcome outcome;
  if (masked)
    outcome.text =
        english ? "The " + match.camera.name +
                      " camera is in privacy mode: Argus is not seeing it. Turn the mask "
                      "off on the camera or in the app to see the picture again."
                : "La cámara " + match.camera.name +
                      " está en modo privacidad: Argus no la ve. Quita el modo privacidad "
                      "en la cámara o en la app para volver a verla.";
  else if (!named)
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
  if (named) {
    arguments["cameraId"] = match.camera.id;
    arguments["privacyState"] =
        cameraPrivacyStateToString(match.camera.privacyState);
    arguments["privacyObeyed"] = match.camera.privacyObeyed;
  }
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

struct PrivacyRequest
{
  CameraCatalog catalog;
  PostureDirectory posture;
  std::shared_ptr<argus::mcp::ConfirmationLedger> ledger;
  argus::mcp::ToolInvocation invocation;
};

drogon::Task<argus::mcp::ToolOutcome> setPrivacy(PrivacyRequest request)
{
  const argus::mcp::ToolInvocation& invocation = request.invocation;
  const bool english = invocation.caller.lang == "en";
  const int64_t userId = invocation.caller.userId;
  if (userId <= 0)
    co_return privacyRefusal(invocation,
                            "No sé de parte de quién viene la orden.",
                            "I do not know who is asking for this.",
                            "unknown_caller");
  const std::string asked = invocation.arguments.get("camera", "").asString();
  const auto cameras = co_await request.catalog();
  if (cameras.empty())
    co_return privacyRefusal(invocation,
                            "Todavía no hay cámaras.",
                            "There are no cameras yet.",
                            "no_cameras");
  const CameraMatch match = matchCamera({.cameras = cameras, .text = asked});
  if (match.kind == CameraMatchKind::Ambiguous)
    co_return privacyRefusal(invocation,
                            "¿A cuál cámara te refieres? " + listed(match.candidates) + ".",
                            "Which camera do you mean? " + listed(match.candidates) + ".",
                            "ambiguous_camera");
  if (match.kind != CameraMatchKind::Exact)
    co_return privacyRefusal(invocation,
                            "No encuentro una cámara llamada " + asked + ". Las cámaras son: " + listed(cameras) + ".",
                            "I could not find a camera called " + asked + ". The cameras are: " + listed(cameras) + ".",
                            "unknown_camera");
  const std::string wanted = invocation.arguments.get("privacy", "").asString();
  const bool unmasking = wanted == "off";
  const HouseholdPosture posture =
      request.posture ? request.posture() : HouseholdPosture{};
  const bool guarded = unmasking ? posture.protects : posture.armed;
  const argus::mcp::ConfirmationKey key{.userId = userId,
                                        .tool = std::string(kSetPrivacyTool),
                                        .target = std::to_string(match.camera.id)};
  const std::string token =
      invocation.arguments.get("confirmation", "").asString();
  const bool obeyed = match.camera.privacyObeyed;
  const bool doubted =
      obeyed && match.camera.privacyState == CameraPrivacyState::Unknown;
  if (unmasking && doubted)
    co_return privacyRefusal(invocation,
                            "No puedo confirmar el estado de la cámara " +
                                match.camera.name +
                                "; la sigo tratando como tapada. Revísala en la app Tapo.",
                            "I cannot confirm the state of the " +
                                match.camera.name +
                                " camera; I keep treating it as masked. Check it in the Tapo app.",
                            "privacy_doubted");
  if (!unmasking && doubted)
    co_return privacyRefusal(invocation,
                            "La cámara " + match.camera.name +
                                " ya se está tratando como tapada: no puedo confirmar su estado, "
                                "así que no la veo ni la grabo.",
                            "The " + match.camera.name +
                                " camera is already being treated as masked: Argus cannot confirm "
                                "its state, so it neither sees nor records it.",
                            "privacy_doubted");
  if (unmasking && !obeyed)
    co_return privacyRefusal(invocation,
                            "La cámara " + match.camera.name + " ya está viendo: su privacidad está apagada.",
                            "The " + match.camera.name + " camera is already seeing: its privacy is off.",
                            "already_off");
  if (!unmasking && obeyed)
    co_return privacyRefusal(invocation,
                            "La cámara " + match.camera.name + " ya está en modo privacidad.",
                            "The " + match.camera.name + " camera is already in privacy mode.",
                            "already_on");
  if (guarded && token.empty()) {
    const std::string code = request.ledger->issue(key);
    argus::mcp::ToolOutcome outcome;
    outcome.text =
        unmasking
            ? (english ? "This would turn off privacy for the " + match.camera.name +
                             " camera while the household is not being watched. Argus "
                             "would see that area again."
                       : "Esto quitaría el modo privacidad de la cámara " +
                             match.camera.name + " mientras la casa no está vigilada. "
                             "Argus volvería a ver esa zona.")
            : (english ? "This would put the " + match.camera.name +
                             " camera in privacy mode while the guard is armed. Argus "
                             "would stop seeing that area."
                       : "Esto pondría la cámara " + match.camera.name +
                             " en modo privacidad con la vigilancia armada. Argus "
                             "dejaría de ver esa zona.");
    outcome.structured["needsConfirmation"] = true;
    outcome.structured["confirmation"] = code;
    outcome.structured["cameraId"] = match.camera.id;
    outcome.structured["camera"] = match.camera.name;
    outcome.structured["privacy"] = wanted;
    co_return outcome;
  }
  if (guarded && !request.ledger->consume(key, token))
    co_return privacyRefusal(invocation,
                            "Ese código no vale (ya se usó, venció o es de otra cámara). Pídele la confirmación otra vez.",
                            "That code is not valid (used, expired or for another camera). Ask for the confirmation again.",
                            "confirmation_invalid");
  CameraControlFeatureService service;
  const auto applied = co_await service.settings(
      {.cameraId = match.camera.id,
       .body = {.privacy = !unmasking,
                .led = std::nullopt,
                .dayNight = std::nullopt,
                .motion = std::nullopt,
                .motionSensitivity = std::nullopt,
                .autoTrack = std::nullopt,
                .alarm = std::nullopt,
                .alarmVolume = std::nullopt},
       .actorUserId = userId,
       .actorIp = {}});
  if (!applied)
    co_return privacyRefusal(invocation,
                            "No encuentro esa cámara.",
                            "I could not find that camera.",
                            "camera_not_found");
  if (!applied->ok) {
    if (applied->failure == "privacy_unsupported")
      co_return privacyRefusal(invocation,
                              "El modelo de la cámara " + match.camera.name +
                                  " no tiene modo privacidad.",
                              "The " + match.camera.name +
                                  " camera model has no privacy mode.",
                              "privacy_unsupported");
    if (applied->failure == "privacy_on")
      co_return privacyRefusal(invocation,
                              "La cámara " + match.camera.name +
                                  " está en modo privacidad y no acepta ese cambio.",
                              "The " + match.camera.name +
                                  " camera is in privacy mode and refused that change.",
                              "privacy_on");
    co_return privacyRefusal(invocation,
                            "La cámara " + match.camera.name + " no aceptó el cambio" +
                                (applied->error.empty() ? std::string{"."} : ": " + applied->error),
                            "The " + match.camera.name + " camera refused the change" +
                                (applied->error.empty() ? std::string{"."} : ": " + applied->error),
                            "privacy_failed");
  }
  const bool obeyedNow = PrivacyStateService::instance().masked(match.camera.id);
  const CameraPrivacyState reportedNow =
      PrivacyStateService::instance().stateOf(match.camera.id);
  if (obeyedNow == unmasking)
    co_return privacyRefusal(invocation,
                            "La cámara " + match.camera.name +
                                " no quedó en el estado que pediste: sigue " +
                                (obeyedNow ? "tratándose como tapada."
                                           : "viendo."),
                            "The " + match.camera.name +
                                " camera did not end up in the state you asked for: it is still " +
                                (obeyedNow ? "treated as masked." : "seeing."),
                            "privacy_not_applied");
  argus::mcp::ToolOutcome outcome;
  outcome.text = unmasking
                     ? (english ? "Privacy is off for the " + match.camera.name +
                                      " camera; Argus sees it again."
                                : "La privacidad de la cámara " + match.camera.name +
                                      " está apagada; Argus vuelve a verla.")
                     : (english ? "Privacy is on for the " + match.camera.name +
                                      " camera; Argus neither sees nor records it."
                                : "La cámara " + match.camera.name +
                                      " está en modo privacidad; Argus no la ve ni la graba.");
  if (reportedNow == CameraPrivacyState::Unknown)
    outcome.text += english
                        ? " The camera did not confirm the change: Argus cannot read "
                          "its lens mask."
                        : " La cámara no confirmó el cambio: Argus no puede leer su "
                          "lente.";
  outcome.structured["cameraId"] = match.camera.id;
  outcome.structured["camera"] = match.camera.name;
  outcome.structured["privacy"] = wanted;
  outcome.structured["privacyState"] = cameraPrivacyStateToString(reportedNow);
  outcome.structured["privacyObeyed"] = obeyedNow;
  outcome.structured["applied"] = true;
  outcome.structured["confirmed"] = reportedNow != CameraPrivacyState::Unknown;
  co_return outcome;
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
  const auto ledger = std::make_shared<argus::mcp::ConfirmationLedger>();
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
  server->add({.name = std::string(kSetPrivacyTool),
               .title = "",
               .description = "Pone o quita el modo privacidad (la lente tapada) de una cámara. privacy es on "
                              "para dejar de ver, grabar y escuchar esa cámara, off para volver a verla. "
                              "camera es el nombre de la cámara",
               .inputSchema = schema::object(
                   {{.name = "camera", .schema = schema::text(), .required = true},
                    {.name = "privacy", .schema = schema::choice({"on", "off"}), .required = true},
                    {.name = "confirmation", .schema = schema::text(), .required = false}}),
               .annotations = {.destructive = true},
               .module = "surveillance",
               .capability = "camera.manage"},
              argus::mcp::onLoop({.loop = input.loop,
                                  .handler = [catalog, posture = input.posture, ledger](
                                                 const argus::mcp::ToolInvocation& invocation) {
                                    return setPrivacy({.catalog = catalog,
                                                       .posture = posture,
                                                       .ledger = ledger,
                                                       .invocation = invocation});
                                  }}));
  server->setGate(tool_gate::capabilities());
  return server;
}
