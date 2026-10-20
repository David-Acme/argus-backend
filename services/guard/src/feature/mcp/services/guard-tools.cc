#include "guard-tools.hxx"

#include <auth/tool-gate.hxx>
#include <mcp/loop-tool.hxx>
#include <mcp/schema.hxx>
#include <text/name-match.hxx>

#include <algorithm>
#include <array>
#include <ctime>
#include <span>
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

struct Phrase
{
  std::string_view key;
  std::string_view es;
  std::string_view en;
};

constexpr std::array<Phrase, 4> kSubjects{{{.key = "stranger", .es = "una persona desconocida", .en = "an unknown person"},
                                           {.key = "several", .es = "varias personas", .en = "several people"},
                                           {.key = "accompanied", .es = "una persona acompañada", .en = "a person with company"},
                                           {.key = "unobserved", .es = "una persona", .en = "a person"}}};

constexpr std::array<Phrase, 4> kDangers{{{.key = "low", .es = "riesgo bajo", .en = "low risk"},
                                          {.key = "medium", .es = "riesgo medio", .en = "medium risk"},
                                          {.key = "high", .es = "riesgo alto", .en = "high risk"},
                                          {.key = "critical", .es = "riesgo crítico", .en = "critical risk"}}};

constexpr std::array<Phrase, 3> kResolutions{{{.key = "recognized", .es = "resuelto: era alguien de casa", .en = "resolved: it was someone from home"},
                                              {.key = "left", .es = "resuelto: se fue", .en = "resolved: they left"},
                                              {.key = "recovered", .es = "resuelto: la cámara volvió a ver", .en = "resolved: the camera sees again"}}};

std::string_view phraseOf(std::span<const Phrase> table, std::string_view key, bool english)
{
  const auto found = std::ranges::find(table, key, &Phrase::key);
  if (found == table.end())
    return {};
  return english ? found->en : found->es;
}

std::string agoText(int64_t seconds, bool english)
{
  constexpr int64_t kMinute = 60;
  constexpr int64_t kHour = 60 * kMinute;
  constexpr int64_t kDay = 24 * kHour;
  if (seconds < kMinute)
    return english ? "just now" : "hace un momento";
  const auto amount = [english](int64_t count, std::string_view es, std::string_view en) {
    return english ? std::to_string(count) + " " + std::string(en) + " ago"
                   : "hace " + std::to_string(count) + " " + std::string(es);
  };
  if (seconds < kHour)
    return amount(seconds / kMinute, "min", "min");
  if (seconds < kDay)
    return amount(seconds / kHour, "h", "h");
  return amount(seconds / kDay, seconds / kDay == 1 ? "día" : "días", seconds / kDay == 1 ? "day" : "days");
}

std::string episodeText(const GuardEpisodeStatus& episode, const GuardStatusLineInput& input)
{
  const bool english = input.english;
  std::string what;
  if (episode.kind == GuardEpisodeKind::Camera) {
    const std::string camera = episode.camera.empty() ? std::string(english ? "a camera" : "una cámara")
                                                      : (english ? "the " + episode.camera + " camera"
                                                                 : "la cámara " + episode.camera);
    what = english ? camera + " lost its picture" : camera + " perdió la imagen";
  }
  else {
    const std::string_view subject = phraseOf(kSubjects, episode.subject, english);
    what = std::string(subject.empty() ? (english ? "a person" : "una persona") : subject);
    what += english ? " at " + input.environment.name : " en " + input.environment.name;
  }
  const std::string_view danger = phraseOf(kDangers, episode.danger, english);
  if (!danger.empty())
    what += " (" + std::string(danger) + ")";
  what += ", " + agoText(std::max<int64_t>(0, input.now - episode.at), english);
  if (!episode.resolved)
    what += english ? ", still open" : ", sigue abierto";
  else {
    const std::string_view resolution = phraseOf(kResolutions, episode.resolution, english);
    what += ", " + std::string(resolution.empty() ? (english ? "resolved" : "resuelto") : resolution);
  }
  return what;
}

drogon::Task<argus::mcp::ToolOutcome> guardStatus(GuardStatusDirectory directory, argus::mcp::ToolInvocation invocation)
{
  const bool english = invocation.caller.lang == "en";
  if (!directory)
    co_return argus::mcp::toolFailure({.text = english ? "The guard status is not available right now."
                                                       : "El estado de la vigilancia no está disponible ahora.",
                                       .code = "status_unavailable"});
  const auto environments = co_await directory();
  argus::mcp::ToolOutcome outcome;
  if (environments.empty()) {
    outcome.text = english ? "There is no guarded place yet." : "Todavía no hay lugares vigilados.";
    outcome.structured["environments"] = Json::Value(Json::arrayValue);
    co_return outcome;
  }
  const auto now = static_cast<int64_t>(std::time(nullptr));
  Json::Value listed(Json::arrayValue);
  for (const auto& environment : environments) {
    outcome.text += (outcome.text.empty() ? "" : "\n") +
                    guardStatusLine({.environment = environment, .english = english, .now = now});
    Json::Value entry(Json::objectValue);
    entry["environmentId"] = static_cast<Json::Int64>(environment.id);
    entry["name"] = environment.name;
    entry["mode"] = environment.mode;
    entry["armed"] = environment.armed;
    if (environment.latest) {
      Json::Value latest(Json::objectValue);
      latest["kind"] = environment.latest->kind == GuardEpisodeKind::Camera ? "camera" : "person";
      latest["danger"] = environment.latest->danger;
      latest["camera"] = environment.latest->camera;
      latest["at"] = static_cast<Json::Int64>(environment.latest->at);
      latest["resolved"] = environment.latest->resolved;
      entry["latest"] = std::move(latest);
    }
    listed.append(std::move(entry));
  }
  outcome.structured["environments"] = std::move(listed);
  co_return outcome;
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

std::string guardStatusLine(const GuardStatusLineInput& input)
{
  const GuardEnvironmentStatus& environment = input.environment;
  const bool english = input.english;
  const auto known = std::ranges::find(kModes, std::string_view(environment.mode), &ModeLabel::value);
  const std::string_view mode = known == kModes.end() ? std::string_view() : (english ? known->en : known->es);
  std::string line = environment.name + ": ";
  line += english ? "mode " : "modo ";
  line += mode.empty() ? environment.mode : std::string(mode);
  line += environment.armed ? (english ? ", armed" : ", vigilancia armada")
                            : (english ? ", not armed" : ", vigilancia desarmada");
  line += english ? ". Latest episode: " : ". Último episodio: ";
  line += environment.latest ? episodeText(*environment.latest, input) : std::string(english ? "none" : "ninguno");
  line += '.';
  return line;
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
  server->add({.name = "guard.status",
               .title = "",
               .description = "Dice el estado de la vigilancia de cada lugar: su modo, si está armada y el último "
                              "episodio (qué pasó, dónde, cuándo y si se resolvió)",
               .inputSchema = schema::object({}),
               .annotations = {.readOnly = true},
               .module = "surveillance",
               .capability = "guard.read"},
              argus::mcp::onLoop({.loop = input.loop, .handler = [status = input.status](const argus::mcp::ToolInvocation& invocation) {
                                    return guardStatus(status, invocation);
                                  }}));
  server->setGate(tool_gate::capabilities());
  return server;
}
