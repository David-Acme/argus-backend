#include "camera-tools.hxx"

#include <auth/tool-gate.hxx>
#include <auth/user-role.hxx>
#include <errors/response-exception.hxx>
#include <feature/camera-control/dtos/camera-ptz-dto.hxx>
#include <feature/camera-control/dtos/camera-settings-dto.hxx>
#include <feature/camera-control/services/camera-control-feature-service.hxx>
#include <shared/services/privacy-state/privacy-state.hxx>
#include <mcp/loop-tool.hxx>
#include <mcp/schema.hxx>
#include <text/name-match.hxx>

#include <algorithm>
#include <array>
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

struct HealthPhrase
{
  std::string_view key;
  std::string_view es;
  std::string_view en;
};

constexpr std::array<HealthPhrase, 11> kHealthPhrases{{
    {.key = "ok", .es = "imagen normal", .en = "picture normal"},
    {.key = "dark", .es = "imagen oscura", .en = "picture dark"},
    {.key = "bright", .es = "imagen demasiado clara", .en = "picture too bright"},
    {.key = "blurred", .es = "imagen borrosa", .en = "picture blurred"},
    {.key = "moved", .es = "la cámara se movió", .en = "the camera was moved"},
    {.key = "unreachable", .es = "sin imagen", .en = "no picture"},
    {.key = "covered", .es = "lente tapado", .en = "lens covered"},
    {.key = "whiteout", .es = "imagen en blanco", .en = "picture washed out"},
    {.key = "blinded", .es = "cámara encandilada", .en = "camera blinded"},
    {.key = "partial_cover", .es = "lente tapado en parte", .en = "lens partly covered"},
    {.key = "dark_sudden", .es = "se oscureció de golpe", .en = "went dark suddenly"},
}};

drogon::Task<argus::mcp::ToolOutcome> cameraStatus(CameraCatalog catalog, argus::mcp::ToolInvocation invocation)
{
  const bool english = invocation.caller.lang == "en";
  const std::string asked = invocation.arguments.get("camera", "").asString();
  const auto cameras = co_await catalog();
  argus::mcp::ToolOutcome outcome;
  if (cameras.empty()) {
    outcome.text = english ? "There are no cameras yet." : "Todavía no hay cámaras.";
    outcome.structured["cameras"] = Json::Value(Json::arrayValue);
    co_return outcome;
  }
  std::vector<CameraChoice> chosen = cameras;
  if (!text_norm::folded(asked).empty()) {
    const CameraMatch match = matchCamera({.cameras = cameras, .text = asked});
    if (match.kind == CameraMatchKind::Ambiguous)
      co_return argus::mcp::toolFailure({.text = english ? "Which camera do you mean? " + listed(match.candidates) + "."
                                                         : "¿A cuál cámara te refieres? " + listed(match.candidates) + ".",
                                         .code = "ambiguous_camera"});
    if (match.kind == CameraMatchKind::Missing)
      co_return argus::mcp::toolFailure(
          {.text = english ? "I could not find a camera called " + asked + ". The cameras are: " + listed(cameras) + "."
                           : "No encuentro una cámara llamada " + asked + ". Las cámaras son: " + listed(cameras) + ".",
           .code = "unknown_camera"});
    chosen = {match.camera};
  }
  Json::Value rows(Json::arrayValue);
  for (const auto& camera : chosen) {
    outcome.text += (outcome.text.empty() ? "" : "\n") + cameraStatusLine({.camera = camera, .english = english});
    Json::Value row(Json::objectValue);
    row["cameraId"] = camera.id;
    row["name"] = camera.name;
    row["online"] = camera.online;
    row["privacyState"] = cameraPrivacyStateToString(camera.privacyState);
    row["privacyObeyed"] = camera.privacyObeyed;
    row["health"] = camera.health.empty() ? std::string("unknown") : camera.health;
    rows.append(std::move(row));
  }
  outcome.structured["cameras"] = std::move(rows);
  co_return outcome;
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
constexpr int64_t kLookPanDegrees = 15;
constexpr int64_t kLookTiltDegrees = 10;

struct PresetChoice
{
  std::string id;
  std::string name;
};

struct PresetWalk
{
  const Json::Value& node;
  std::vector<PresetChoice>& out;
  int depth{0};
};

void collectPresets(const PresetWalk& walk)
{
  const Json::Value& node = walk.node;
  std::vector<PresetChoice>& out = walk.out;
  if (!node.isObject() || walk.depth > 8 || !out.empty())
    return;
  const Json::Value& ids = node["id"];
  const Json::Value& names = node["name"];
  if (ids.isArray() && names.isArray() && ids.size() == names.size()) {
    for (Json::ArrayIndex index = 0; index < ids.size(); ++index)
      out.push_back({.id = ids[index].isString() ? ids[index].asString() : std::to_string(ids[index].asInt64()),
                     .name = names[index].asString()});
    return;
  }
  for (const auto& key : node.getMemberNames())
    collectPresets({.node = node[key], .out = out, .depth = walk.depth + 1});
}

std::string_view aimRefusalText(const ResponseException& error, bool english)
{
  if (error.errorCode() == "GUARD_OFF_DUTY")
    return english ? "You can move the camera only while you are on duty."
                   : "Solo puedes mover la cámara mientras estás de turno.";
  if (error.errorCode() == "PRESETS_NOT_YOURS")
    return english ? "Only the owner or a resident changes the camera positions."
                   : "Solo el propietario o un residente cambia las posiciones de la cámara.";
  return english ? "I could not move the camera." : "No pude mover la cámara.";
}

struct AimFailure
{
  const CameraControlResult& result;
  const std::string& camera;
  bool english{false};
};

std::string aimFailureText(const AimFailure& failure)
{
  const CameraControlResult& result = failure.result;
  const std::string& camera = failure.camera;
  const bool english = failure.english;
  if (!result)
    return english ? "I could not find that camera." : "No encuentro esa cámara.";
  if (result->failure == "privacy_on")
    return english ? "The " + camera + " camera is in privacy mode; Argus does not move it."
                   : "La cámara " + camera + " está en modo privacidad; Argus no la mueve.";
  if (result->failure == "no_home")
    return english ? "The " + camera + " camera has no home position yet."
                   : "La cámara " + camera + " todavía no tiene posición de vigilancia.";
  return english ? "The " + camera + " camera did not move" + (result->error.empty() ? "." : ": " + result->error)
                 : "La cámara " + camera + " no se movió" + (result->error.empty() ? "." : ": " + result->error);
}

struct AimRequest
{
  CameraCatalog catalog;
  argus::mcp::ToolInvocation invocation;
};

drogon::Task<std::optional<CameraChoice>> aimedCamera(const AimRequest& request, argus::mcp::ToolOutcome& refusal)
{
  const bool english = request.invocation.caller.lang == "en";
  const std::string asked = request.invocation.arguments.get("camera", "").asString();
  const auto cameras = co_await request.catalog();
  if (cameras.empty()) {
    refusal = argus::mcp::toolFailure(
        {.text = english ? "There are no cameras yet." : "Todavía no hay cámaras.", .code = "no_cameras"});
    co_return std::nullopt;
  }
  if (text_norm::folded(asked).empty() && cameras.size() == 1)
    co_return cameras.front();
  const CameraMatch match = matchCamera({.cameras = cameras, .text = asked});
  if (match.kind == CameraMatchKind::Exact)
    co_return match.camera;
  refusal = argus::mcp::toolFailure(
      {.text = match.kind == CameraMatchKind::Ambiguous
                   ? (english ? "Which camera do you mean? " + listed(match.candidates) + "."
                              : "¿A cuál cámara te refieres? " + listed(match.candidates) + ".")
                   : (english ? "Which camera? The cameras are: " + listed(cameras) + "."
                              : "¿Qué cámara? Las cámaras son: " + listed(cameras) + "."),
       .code = match.kind == CameraMatchKind::Ambiguous ? "ambiguous_camera" : "unknown_camera"});
  co_return std::nullopt;
}

bool homeWord(const std::string& text)
{
  const std::string folded = text_norm::folded(text);
  return folded == "home" || folded == "casa" || folded == "vigilancia" || folded == "inicio" ||
         folded == "posicion de vigilancia";
}

drogon::Task<argus::mcp::ToolOutcome> gotoPreset(AimRequest request)
{
  const argus::mcp::ToolInvocation& invocation = request.invocation;
  const bool english = invocation.caller.lang == "en";
  argus::mcp::ToolOutcome refusal;
  const auto camera = co_await aimedCamera(request, refusal);
  if (!camera)
    co_return refusal;
  const UserRole role = parseUserRole(invocation.caller.role).value_or(UserRole::Unknown);
  const std::string wanted = invocation.arguments.get("preset", "").asString();
  const CameraControlFeatureService service;
  try {
    CameraControlResult result;
    if (homeWord(wanted)) {
      result = co_await service.preset({.cameraId = camera->id,
                                        .body = {.action = "go_home", .id = {}, .name = {}},
                                        .actorUserId = invocation.caller.userId,
                                        .actorRole = role});
    }
    else {
      const auto listed = co_await service.presets(camera->id);
      std::vector<PresetChoice> presets;
      if (listed && listed->ok)
        collectPresets({.node = listed->data, .out = presets, .depth = 0});
      std::vector<std::string> names;
      names.reserve(presets.size());
      for (const auto& preset : presets)
        names.push_back(preset.name);
      const auto found = text_norm::matchName(names, wanted);
      if (found.kind != text_norm::NameMatchKind::Exact) {
        std::string known;
        for (const auto& name : names)
          known += (known.empty() ? "" : ", ") + name;
        co_return argus::mcp::toolFailure(
            {.text = english ? "The " + camera->name + " camera has no position called " + wanted +
                                   (known.empty() ? "." : ". Its positions are: " + known + ".")
                             : "La cámara " + camera->name + " no tiene una posición llamada " + wanted +
                                   (known.empty() ? "." : ". Sus posiciones son: " + known + "."),
             .code = "unknown_preset"});
      }
      const PresetChoice& preset = presets.at(found.hits.front());
      result = co_await service.preset({.cameraId = camera->id,
                                        .body = {.action = "goto", .id = preset.id, .name = preset.name},
                                        .actorUserId = invocation.caller.userId,
                                        .actorRole = role});
    }
    if (!result || !result->ok)
      co_return argus::mcp::toolFailure({.text = aimFailureText({.result = result, .camera = camera->name, .english = english}), .code = "aim_failed"});
  }
  catch (const ResponseException& error) {
    co_return argus::mcp::toolFailure({.text = std::string(aimRefusalText(error, english)), .code = error.errorCode()});
  }
  argus::mcp::ToolOutcome outcome;
  outcome.text = english ? "The " + camera->name + " camera turned to " + wanted + "."
                         : "La cámara " + camera->name + " giró hacia " + wanted + ".";
  outcome.structured["cameraId"] = camera->id;
  outcome.structured["camera"] = camera->name;
  outcome.structured["preset"] = wanted;
  co_return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> lookDirection(AimRequest request)
{
  const argus::mcp::ToolInvocation& invocation = request.invocation;
  const bool english = invocation.caller.lang == "en";
  argus::mcp::ToolOutcome refusal;
  const auto camera = co_await aimedCamera(request, refusal);
  if (!camera)
    co_return refusal;
  const UserRole role = parseUserRole(invocation.caller.role).value_or(UserRole::Unknown);
  const std::string direction = invocation.arguments.get("direction", "").asString();
  const CameraControlFeatureService service;
  try {
    CameraControlResult result;
    if (direction == "home") {
      result = co_await service.preset({.cameraId = camera->id,
                                        .body = {.action = "go_home", .id = {}, .name = {}},
                                        .actorUserId = invocation.caller.userId,
                                        .actorRole = role});
    }
    else {
      CameraPtzDto body;
      body.x = direction == "left" ? -kLookPanDegrees : direction == "right" ? kLookPanDegrees : 0;
      body.y = direction == "up" ? kLookTiltDegrees : direction == "down" ? -kLookTiltDegrees : 0;
      result = co_await service.move(
          {.cameraId = camera->id, .body = body, .actorUserId = invocation.caller.userId, .actorRole = role});
    }
    if (!result || !result->ok)
      co_return argus::mcp::toolFailure({.text = aimFailureText({.result = result, .camera = camera->name, .english = english}), .code = "aim_failed"});
  }
  catch (const ResponseException& error) {
    co_return argus::mcp::toolFailure({.text = std::string(aimRefusalText(error, english)), .code = error.errorCode()});
  }
  argus::mcp::ToolOutcome outcome;
  outcome.text = direction == "home"
                     ? (english ? "The " + camera->name + " camera is back at its home position."
                                : "La cámara " + camera->name + " volvió a su posición de vigilancia.")
                     : (english ? "I moved the " + camera->name + " camera." : "Moví la cámara " + camera->name + ".");
  outcome.structured["cameraId"] = camera->id;
  outcome.structured["camera"] = camera->name;
  outcome.structured["direction"] = direction;
  co_return outcome;
}
}

std::string cameraStatusLine(const CameraStatusLineInput& input)
{
  const CameraChoice& camera = input.camera;
  const bool english = input.english;
  std::string line = english ? "The " + camera.name + " camera: " : "Cámara " + camera.name + ": ";
  line += camera.online ? (english ? "online" : "en línea") : (english ? "offline" : "fuera de línea");
  if (camera.privacyObeyed && camera.privacyState == CameraPrivacyState::Unknown)
    line += english ? ", privacy unconfirmed, treated as masked" : ", privacidad sin confirmar, se trata como tapada";
  else if (camera.privacyObeyed)
    line += english ? ", privacy mode on (Argus does not see it)" : ", modo privacidad encendido (Argus no la ve)";
  else
    line += english ? ", privacy mode off" : ", modo privacidad apagado";
  const auto health = std::ranges::find(kHealthPhrases, std::string_view(camera.health), &HealthPhrase::key);
  if (health != kHealthPhrases.end() && camera.online)
    line += ", " + std::string(english ? health->en : health->es);
  line += ".";
  return line;
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
  server->add({.name = "camera.goto_preset",
               .title = "",
               .description = "Gira una cámara a una de sus posiciones guardadas, por su nombre. camera es la "
                              "cámara (vacío si solo hay una) y preset el nombre de la posición; casa o vigilancia "
                              "la devuelven a su posición de vigilancia",
               .inputSchema = schema::object({{.name = "camera", .schema = schema::text(), .required = false},
                                              {.name = "preset", .schema = schema::text(), .required = true}}),
               .annotations = {},
               .module = "surveillance",
               .capability = "camera.aim"},
              argus::mcp::onLoop({.loop = input.loop, .handler = [catalog](const argus::mcp::ToolInvocation& invocation) {
                                    return gotoPreset({.catalog = catalog, .invocation = invocation});
                                  }}));
  server->add({.name = "camera.look",
               .title = "",
               .description = "Mueve una cámara un paso hacia una dirección: left, right, up o down; home la "
                              "devuelve a su posición de vigilancia",
               .inputSchema = schema::object(
                   {{.name = "camera", .schema = schema::text(), .required = false},
                    {.name = "direction", .schema = schema::choice({"left", "right", "up", "down", "home"}),
                     .required = true}}),
               .annotations = {},
               .module = "surveillance",
               .capability = "camera.aim"},
              argus::mcp::onLoop({.loop = input.loop, .handler = [catalog](const argus::mcp::ToolInvocation& invocation) {
                                    return lookDirection({.catalog = catalog, .invocation = invocation});
                                  }}));
  server->add({.name = "camera.status",
               .title = "",
               .description = "Dice el estado de las cámaras: si están en línea, si tienen el modo privacidad y cómo "
                              "está su imagen. camera es el nombre de una cámara; vacío las dice todas",
               .inputSchema = schema::object({{.name = "camera", .schema = schema::text(), .required = false}}),
               .annotations = {.readOnly = true},
               .module = "surveillance",
               .capability = "camera.view"},
              argus::mcp::onLoop({.loop = input.loop, .handler = [catalog](const argus::mcp::ToolInvocation& invocation) {
                                    return cameraStatus(catalog, invocation);
                                  }}));
  server->setGate(tool_gate::capabilities());
  return server;
}
