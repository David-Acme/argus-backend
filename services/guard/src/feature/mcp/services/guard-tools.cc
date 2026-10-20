#include "guard-tools.hxx"

#include <auth/tool-gate.hxx>
#include <mcp/loop-tool.hxx>
#include <mcp/schema.hxx>
#include <text/name-match.hxx>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace
{
namespace schema = argus::mcp::schema;

struct ModeLabel
{
  std::string_view value;
  std::string_view es;
  std::string_view en;
};

constexpr std::array<ModeLabel, 4> kModes{{{.value = "home", .es = "en casa", .en = "home"},
                                           {.value = "night", .es = "noche", .en = "night"},
                                           {.value = "away", .es = "fuera de casa", .en = "away"},
                                           {.value = "armed", .es = "máxima alerta", .en = "armed"}}};

std::string modeLabel(const argus::mcp::ToolInvocation& invocation)
{
  std::string value = invocation.arguments.get("mode", "").asString();
  const auto found = std::ranges::find(kModes, std::string_view(value), &ModeLabel::value);
  if (found == kModes.end())
    return value;
  return std::string(invocation.caller.lang == "en" ? found->en : found->es);
}

std::string listed(const std::vector<EnvironmentChoice>& places)
{
  std::string out;
  for (const auto& place : places)
    out += (out.empty() ? "" : ", ") + place.name;
  return out;
}

bool expectsProtection(std::string_view mode)
{
  return mode == "away" || mode == "armed" || mode == "night";
}

std::string maskedSentence(const std::vector<MaskedCamera>& masked, bool english)
{
  if (masked.empty())
    return {};
  std::string names;
  for (const auto& camera : masked) {
    const std::string label = camera.name.empty()
                                  ? std::to_string(camera.cameraId)
                                  : camera.name;
    names += (names.empty() ? "" : ", ") + label;
  }
  if (masked.size() == 1)
    return english ? " Careful: the " + names +
                         " camera is in privacy mode and does not see anything."
                   : " Ojo: la cámara " + names +
                         " está en modo privacidad y no ve nada.";
  return english ? " Careful: " + names +
                       " are in privacy mode and do not see anything."
                 : " Ojo: " + names +
                       " están en modo privacidad y no ven nada.";
}

argus::mcp::ToolOutcome changed(const argus::mcp::ToolInvocation& invocation, const EnvironmentChoice* place,
                                const std::vector<MaskedCamera>& masked)
{
  const bool english = invocation.caller.lang == "en";
  const std::string mode = invocation.arguments.get("mode", "").asString();
  argus::mcp::ToolOutcome outcome;
  Json::Value arguments(Json::objectValue);
  arguments["mode"] = mode;
  if (place != nullptr) {
    arguments["environment"] = place->name;
    arguments["environmentId"] = place->id;
    outcome.text = english ? "The app set the guard mode of " + place->name + " to " + modeLabel(invocation) + "."
                           : "La app puso la vigilancia de " + place->name + " en modo " + modeLabel(invocation) + ".";
  }
  else {
    outcome.text = english ? "The app set the guard mode to " + modeLabel(invocation) + "."
                           : "La app puso la vigilancia en modo " + modeLabel(invocation) + ".";
  }
  if (expectsProtection(mode)) {
    const std::string warning = maskedSentence(masked, english);
    outcome.text += warning;
    if (!masked.empty()) {
      Json::Value listed(Json::arrayValue);
      for (const auto& camera : masked) {
        Json::Value entry(Json::objectValue);
        entry["cameraId"] = static_cast<Json::Int64>(camera.cameraId);
        entry["name"] = camera.name;
        listed.append(std::move(entry));
      }
      arguments["maskedCameras"] = std::move(listed);
    }
  }
  outcome.structured = arguments;
  outcome.appAction = argus::mcp::AppAction{.name = "app.set_guard_mode", .arguments = std::move(arguments)};
  return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> setGuardMode(EnvironmentCatalog catalog, MaskedCameraDirectory maskedCameras,
                                                   argus::mcp::ToolInvocation invocation)
{
  const std::string asked = invocation.arguments.get("environment", "").asString();
  const std::vector<MaskedCamera> masked =
      maskedCameras ? maskedCameras() : std::vector<MaskedCamera>{};
  if (text_norm::folded(asked).empty())
    co_return changed(invocation, nullptr, masked);
  const auto places = co_await catalog();
  std::vector<std::string> names;
  names.reserve(places.size());
  for (const auto& place : places)
    names.push_back(place.name);
  const auto match = text_norm::matchName(names, asked);
  const bool english = invocation.caller.lang == "en";
  if (match.kind == text_norm::NameMatchKind::Exact)
    co_return changed(invocation, &places.at(match.hits.front()), masked);
  if (match.kind == text_norm::NameMatchKind::Ambiguous) {
    std::vector<EnvironmentChoice> candidates;
    candidates.reserve(match.hits.size());
    for (const size_t index : match.hits)
      candidates.push_back(places.at(index));
    co_return argus::mcp::toolFailure({.text = english ? "Which place do you mean? " + listed(candidates) + "."
                              : "¿A cuál lugar te refieres? " + listed(candidates) + ".", .code = "ambiguous_environment"});
  }
  co_return argus::mcp::toolFailure({.text = english ? "I could not find a place called " + asked + ". The places are: " + listed(places) + "."
                            : "No encuentro un lugar llamado " + asked + ". Los lugares son: " + listed(places) + ".", .code = "unknown_environment"});
}

drogon::Task<argus::mcp::ToolOutcome> maintenanceCall(MaintenanceVoice port, argus::mcp::ToolInvocation invocation)
{
  const bool english = invocation.caller.lang == "en";
  if (!port)
    co_return argus::mcp::toolFailure({.text = english ? "Maintenance is not available right now."
                                                       : "El mantenimiento no está disponible ahora.",
                                       .code = "maintenance_unavailable"});
  const MaintenanceVoiceReply reply = co_await port({.userId = invocation.caller.userId,
                                                     .role = invocation.caller.role,
                                                     .lang = invocation.caller.lang,
                                                     .camera = invocation.arguments.get("camera", "").asString(),
                                                     .minutes = invocation.arguments.get("minutes", 0).asInt64()});
  if (!reply.ok)
    co_return argus::mcp::toolFailure({.text = reply.text, .code = reply.code});
  argus::mcp::ToolOutcome outcome;
  outcome.text = reply.text;
  outcome.structured = reply.structured;
  co_return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> whereaboutsCall(WhereaboutsVoice port, argus::mcp::ToolInvocation invocation)
{
  const bool english = invocation.caller.lang == "en";
  if (!port)
    co_return argus::mcp::toolFailure({.text = english ? "I cannot tell where anyone is right now."
                                                       : "Ahora no puedo saber dónde está nadie.",
                                       .code = "whereabouts_unavailable"});
  const WhereaboutsVoiceReply reply = co_await port({.userId = invocation.caller.userId,
                                                     .role = invocation.caller.role,
                                                     .lang = invocation.caller.lang,
                                                     .person = invocation.arguments.get("person", "").asString(),
                                                     .room = invocation.arguments.get("room", "").asString()});
  if (!reply.ok)
    co_return argus::mcp::toolFailure({.text = reply.text, .code = reply.code});
  argus::mcp::ToolOutcome outcome;
  outcome.text = reply.text;
  outcome.structured = reply.structured;
  co_return outcome;
}

}

std::shared_ptr<argus::mcp::McpServer> guardToolServer(const GuardToolsInput& input)
{
  auto server = std::make_shared<argus::mcp::McpServer>(
      argus::mcp::ServerIdentity{.name = "argus-guard", .version = "1", .instructions = ""});
  const EnvironmentCatalog catalog = input.catalog;
  const MaskedCameraDirectory maskedCameras = input.maskedCameras;
  server->add({.name = "app.set_guard_mode",
               .title = "",
               .description = "Cambia el modo de vigilancia: home al estar en casa, night al dormir, away al salir, "
                              "armed para máxima alerta. environment es el nombre del lugar (casa, restaurante, "
                              "oficina...) si el usuario nombra uno; vacío cambia todos",
               .inputSchema = schema::object(
                   {{.name = "mode", .schema = schema::choice({"home", "night", "away", "armed"}), .required = true},
                    {.name = "environment", .schema = schema::text(), .required = false}}),
               .annotations = {.idempotent = true},
               .module = "surveillance",
               .capability = "guard.mode.set"},
              argus::mcp::onLoop({.loop = input.loop, .handler = [catalog, maskedCameras](const argus::mcp::ToolInvocation& invocation) {
                return setGuardMode(catalog, maskedCameras, invocation);
              }}));
  const MaintenanceVoicePorts maintenance = input.maintenance;
  server->add({.name = "guard.maintenance_start",
               .title = "",
               .description = "Abre una ventana de mantenimiento de cámaras: mientras dure, tapar, mover o limpiar "
                              "esas cámaras no es una alarma. camera es el nombre de la cámara (vacío para todas las "
                              "del lugar) y minutes la duración (15 por defecto, 120 como máximo)",
               .inputSchema = schema::object({{.name = "camera", .schema = schema::text(), .required = false},
                                              {.name = "minutes", .schema = schema::integer(), .required = false}}),
               .annotations = {},
               .module = "surveillance",
               .capability = "guard.maintenance"},
              argus::mcp::onLoop({.loop = input.loop, .handler = [maintenance](const argus::mcp::ToolInvocation& invocation) {
                                    return maintenanceCall(maintenance.start, invocation);
                                  }}));
  server->add({.name = "guard.maintenance_end",
               .title = "",
               .description = "Cierra la ventana de mantenimiento abierta por quien habla (\"ya terminé\")",
               .inputSchema = schema::object({}),
               .annotations = {.idempotent = true},
               .module = "surveillance",
               .capability = "guard.maintenance"},
              argus::mcp::onLoop({.loop = input.loop, .handler = [maintenance](const argus::mcp::ToolInvocation& invocation) {
                                    return maintenanceCall(maintenance.end, invocation);
                                  }}));
  server->add({.name = "guard.maintenance_status",
               .title = "",
               .description = "Dice qué ventanas de mantenimiento están abiertas y qué cámaras perdieron la imagen "
                              "hace poco, para contestar por qué avisó Argus o quién movió una cámara",
               .inputSchema = schema::object({}),
               .annotations = {.readOnly = true},
               .module = "surveillance",
               .capability = "guard.maintenance"},
              argus::mcp::onLoop({.loop = input.loop, .handler = [maintenance](const argus::mcp::ToolInvocation& invocation) {
                                    return maintenanceCall(maintenance.status, invocation);
                                  }}));
  const WhereaboutsVoice whereabouts = input.whereabouts;
  server->add({.name = "guard.where_is",
               .title = "",
               .description = "Dice dónde está alguien de casa: la habitación en la que Argus le ubica, o si está en "
                              "casa o fuera, y qué tan seguro está. person es el nombre de la persona; room es una "
                              "habitación (sala, cocina...) para saber quién está ahí; vacíos los dos, quién está en "
                              "casa. Solo cuenta a quien comparte su ubicación",
               .inputSchema = schema::object({{.name = "person", .schema = schema::text(), .required = false},
                                              {.name = "room", .schema = schema::text(), .required = false}}),
               .annotations = {.readOnly = true},
               .module = "surveillance",
               .capability = "presence.read"},
              argus::mcp::onLoop({.loop = input.loop, .handler = [whereabouts](const argus::mcp::ToolInvocation& invocation) {
                                    return whereaboutsCall(whereabouts, invocation);
                                  }}));
  server->setGate(tool_gate::capabilities());
  return server;
}
