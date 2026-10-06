#include "module-tools.hxx"

#include <auth/tool-gate.hxx>
#include <mcp/confirmation.hxx>
#include <mcp/loop-tool.hxx>
#include <mcp/schema.hxx>
#include <mcp/speech.hxx>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace
{
namespace schema = argus::mcp::schema;
namespace speech = argus::mcp::speech;

constexpr const char* kCore = "core";
constexpr std::string_view kDisableTool = "modules.disable";

struct EffectPhrase
{
  std::string_view kind;
  std::string_view es;
  std::string_view en;
};

constexpr std::array<EffectPhrase, 9> kEffects{
    {{.kind = "live_views", .es = "las imágenes en vivo", .en = "live views"},
     {.kind = "camera_talk", .es = "hablar por las cámaras", .en = "talking through cameras"},
     {.kind = "camera_detection", .es = "la detección de las cámaras", .en = "camera detection"},
     {.kind = "guard_evaluation", .es = "la evaluación de la vigilancia", .en = "the guard's evaluation"},
     {.kind = "guard_digests", .es = "los resúmenes de la vigilancia", .en = "the guard digests"},
     {.kind = "guard_duty", .es = "las guardias en curso", .en = "guard duty in progress"},
     {.kind = "pending_alerts", .es = "los avisos pendientes", .en = "pending alerts"},
     {.kind = "visitor_recognition", .es = "el reconocimiento de visitantes", .en = "visitor recognition"},
     {.kind = "agenda_calls", .es = "las llamadas de la agenda", .en = "agenda calls"}}};

std::string phraseOf(const ImpactStop& stop, bool english)
{
  const auto found = std::ranges::find(kEffects, std::string_view(stop.kind), &EffectPhrase::kind);
  std::string phrase;
  if (found != kEffects.end()) {
    phrase = std::string(english ? found->en : found->es);
  }
  else {
    phrase = stop.kind;
    std::ranges::replace(phrase, '_', ' ');
  }
  if (stop.count && *stop.count > 0)
    phrase += " (" + std::to_string(*stop.count) + ")";
  return phrase;
}

std::string stateWord(const ModuleCard& card, bool english)
{
  switch (card.state) {
    case ModuleState::Active:
      return english ? "on" : "activo";
    case ModuleState::Off:
      return english ? "off" : "apagado";
    case ModuleState::Installing:
      return std::string(english ? "installing " : "instalándose ") + std::to_string(static_cast<int>(card.progress * 100)) + "%";
    case ModuleState::ComingSoon:
      return english ? "coming soon" : "próximamente";
  }
  return {};
}

std::string moduleArgument(const argus::mcp::ToolInvocation& invocation)
{
  return invocation.arguments.get("module", "").asString();
}

DeskLookup lookupOf(const argus::mcp::ToolInvocation& invocation)
{
  return {.moduleId = moduleArgument(invocation), .lang = std::string(speech::languageOf(invocation))};
}

DeskCommand commandOf(const argus::mcp::ToolInvocation& invocation)
{
  return {.moduleId = moduleArgument(invocation),
          .userId = invocation.caller.userId,
          .lang = std::string(speech::languageOf(invocation))};
}

argus::mcp::ToolOutcome unknownModule(const argus::mcp::ToolInvocation& invocation)
{
  return speech::refuse({.invocation = invocation,
                         .spanish = "No conozco un módulo llamado " + moduleArgument(invocation) + ".",
                         .english = "I do not know a module called " + moduleArgument(invocation) + "."},
                        "unknown_module");
}

argus::mcp::ToolOutcome unavailable(const argus::mcp::ToolInvocation& invocation)
{
  return speech::refuse({.invocation = invocation,
                         .spanish = "Los módulos no responden ahora mismo. Inténtalo en un momento.",
                         .english = "The modules are not answering right now. Try again in a moment."},
                        "unavailable");
}

Json::Value cardJson(const ModuleCard& card)
{
  Json::Value out(Json::objectValue);
  out["id"] = card.id;
  out["name"] = card.name;
  out["active"] = card.state == ModuleState::Active;
  out["comingSoon"] = card.state == ModuleState::ComingSoon;
  return out;
}

drogon::Task<argus::mcp::ToolOutcome> listModules(std::shared_ptr<ModuleDesk> desk, argus::mcp::ToolInvocation invocation)
{
  const bool english = speech::inEnglish(invocation);
  const auto cards = co_await desk->list(std::string(speech::languageOf(invocation)));
  if (cards.empty())
    co_return unavailable(invocation);
  std::vector<std::string> spoken;
  Json::Value listed(Json::arrayValue);
  for (const auto& card : cards) {
    spoken.push_back(card.name + " (" + stateWord(card, english) + ")");
    listed.append(cardJson(card));
  }
  argus::mcp::ToolOutcome outcome;
  outcome.text = (english ? "The Argus modules: " : "Los módulos de Argus: ") + speech::joined(spoken, speech::languageOf(invocation)) + ".";
  outcome.structured["modules"] = std::move(listed);
  co_return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> explainModule(std::shared_ptr<ModuleDesk> desk, argus::mcp::ToolInvocation invocation)
{
  const bool english = speech::inEnglish(invocation);
  const auto card = co_await desk->find(lookupOf(invocation));
  if (!card)
    co_return unknownModule(invocation);
  std::string text = card->name + ": " + (card->what.empty() ? card->summary : card->what);
  if (!card->examples.empty())
    text += std::string(english ? " For example: " : " Por ejemplo: ") + speech::joined(card->examples, speech::languageOf(invocation)) + ".";
  text += std::string(english ? " It is " : " Ahora está ") + stateWord(*card, english) + ".";
  argus::mcp::ToolOutcome outcome;
  outcome.text = std::move(text);
  outcome.structured = cardJson(*card);
  co_return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> requestModule(std::shared_ptr<ModuleDesk> desk, argus::mcp::ToolInvocation invocation)
{
  const auto card = co_await desk->find(lookupOf(invocation));
  if (!card)
    co_return unknownModule(invocation);
  const auto outcome = co_await desk->request(commandOf(invocation));
  argus::mcp::ToolOutcome result;
  switch (outcome.kind) {
    case RequestKind::Requested:
      result.text = speech::say({.invocation = invocation,
                                 .spanish = "Le avisé al dueño de la casa para que active " + card->name + ".",
                                 .english = "I let the owner of the house know so they can turn on " + card->name + "."});
      result.structured["requested"] = true;
      co_return result;
    case RequestKind::Duplicate:
      result.text = speech::say({.invocation = invocation,
                                 .spanish = "Ya le había avisado hoy al dueño sobre " + card->name + ".",
                                 .english = "I already told the owner about " + card->name + " today."});
      result.structured["requested"] = true;
      result.structured["duplicate"] = true;
      co_return result;
    case RequestKind::AlreadyActive:
      co_return speech::refuse({.invocation = invocation,
                                .spanish = card->name + " ya está activo.",
                                .english = card->name + " is already on."},
                               "already_active");
    case RequestKind::ComingSoon:
      co_return speech::refuse({.invocation = invocation,
                                .spanish = card->name + " todavía no está disponible.",
                                .english = card->name + " is not available yet."},
                               "coming_soon");
    case RequestKind::Unknown:
      co_return unknownModule(invocation);
    case RequestKind::Unavailable:
      break;
  }
  co_return unavailable(invocation);
}

drogon::Task<argus::mcp::ToolOutcome> enableModule(std::shared_ptr<ModuleDesk> desk, argus::mcp::ToolInvocation invocation)
{
  const auto card = co_await desk->find(lookupOf(invocation));
  if (!card)
    co_return unknownModule(invocation);
  const auto outcome = co_await desk->enable(commandOf(invocation));
  argus::mcp::ToolOutcome result;
  switch (outcome.kind) {
    case EnableKind::Started:
      result.text = speech::say({.invocation = invocation,
                                 .spanish = "Estoy activando " + card->name + "; te aviso cuando esté lista.",
                                 .english = "I am turning on " + card->name + "; I will tell you when it is ready."});
      result.structured["started"] = true;
      co_return result;
    case EnableKind::AlreadyActive:
      result.text = speech::say({.invocation = invocation,
                                 .spanish = card->name + " ya está activo.",
                                 .english = card->name + " is already on."});
      result.structured["alreadyActive"] = true;
      co_return result;
    case EnableKind::ComingSoon:
      co_return speech::refuse({.invocation = invocation,
                                .spanish = card->name + " todavía no está disponible.",
                                .english = card->name + " is not available yet."},
                               "coming_soon");
    case EnableKind::HardwareInsufficient:
      co_return speech::refuse({.invocation = invocation,
                                .spanish = "Este equipo no alcanza para " + card->name + (outcome.detail.empty() ? "" : ": " + outcome.detail) + ".",
                                .english = "This machine cannot run " + card->name + (outcome.detail.empty() ? "" : ": " + outcome.detail) + "."},
                               "hardware_insufficient");
    case EnableKind::JobRunning:
      co_return speech::refuse({.invocation = invocation,
                                .spanish = card->name + " ya se está instalando.",
                                .english = card->name + " is already being installed."},
                               "job_running");
    case EnableKind::Unknown:
      co_return unknownModule(invocation);
    case EnableKind::Unavailable:
      break;
  }
  co_return unavailable(invocation);
}

std::string impactText(const ModuleCard& card, const ModuleImpact& impact, const argus::mcp::ToolInvocation& invocation)
{
  const bool english = speech::inEnglish(invocation);
  const auto language = speech::languageOf(invocation);
  std::string text = (english ? "Turning off " : "Apagar ") + card.name + (english ? " would stop " : " detendría ");
  std::vector<std::string> stops;
  stops.reserve(impact.stops.size());
  for (const auto& stop : impact.stops)
    stops.push_back(phraseOf(stop, english));
  text += stops.empty() ? std::string(english ? "nothing that is running" : "nada que esté en marcha") : speech::joined(stops, language);
  text += '.';
  if (!impact.holders.empty()) {
    std::vector<std::string> names;
    names.reserve(impact.holders.size());
    for (const auto& holder : impact.holders)
      names.push_back(holder.name + " (" + holder.role + ")");
    text += english ? " Their role would become inactive for: " : " Su rol quedaría inactivo para: ";
    text += speech::joined(names, language) + ".";
  }
  if (!impact.invitations.empty())
    text += (english ? " Pending invitations that would be revoked: " : " Invitaciones pendientes que se revocarían: ") +
            std::to_string(impact.invitations.size()) + ".";
  for (const auto& note : impact.keepsRunning) {
    const std::string& line = english ? (note.english.empty() ? note.spanish : note.english)
                                      : (note.spanish.empty() ? note.english : note.spanish);
    if (line.empty())
      continue;
    text += " " + line;
    if (!line.ends_with('.'))
      text += '.';
  }
  text += english ? " Nothing is deleted; you can turn it on again whenever you want."
                  : " No se borra nada; puedes volver a activarlo cuando quieras.";
  return text;
}

drogon::Task<argus::mcp::ToolOutcome> disableModule(std::shared_ptr<ModuleDesk> desk,
                                                    std::shared_ptr<argus::mcp::ConfirmationLedger> ledger,
                                                    argus::mcp::ToolInvocation invocation)
{
  const auto card = co_await desk->find(lookupOf(invocation));
  if (!card)
    co_return unknownModule(invocation);
  const argus::mcp::ConfirmationKey key{.userId = invocation.caller.userId, .tool = std::string(kDisableTool), .target = card->id};
  const std::string token = invocation.arguments.get("confirmation", "").asString();
  if (token.empty()) {
    const auto impact = co_await desk->impact(lookupOf(invocation));
    if (!impact.known)
      co_return unknownModule(invocation);
    if (!impact.allowed)
      co_return speech::refuse({.invocation = invocation,
                                .spanish = "No se puede apagar " + card->name + ": " + impact.refusal,
                                .english = "Cannot turn off " + card->name + ": " + impact.refusal},
                               impact.refusalCode.empty() ? "refused" : impact.refusalCode);
    const std::string code = ledger->issue(key);
    argus::mcp::ToolOutcome preview;
    preview.text = impactText(*card, impact, invocation) +
                   speech::say({.invocation = invocation,
                                .spanish = " Pídele al usuario que lo confirme; si dice que sí, llama otra vez a modules.disable "
                                           "con module=" + card->id + " y confirmation=" + code + ".",
                                .english = " Ask the user to confirm; if they say yes, call modules.disable again with module=" +
                                           card->id + " and confirmation=" + code + "."});
    preview.structured["needsConfirmation"] = true;
    preview.structured["module"] = card->id;
    co_return preview;
  }
  if (!ledger->consume(key, token))
    co_return speech::refuse({.invocation = invocation,
                              .spanish = "Ese código no vale (ya se usó, venció o es de otro módulo). Vuelve a pedirle la confirmación al usuario.",
                              .english = "That code is not valid (used, expired or for another module). Ask the user to confirm again."},
                             "confirmation_invalid");
  const auto outcome = co_await desk->disable(commandOf(invocation));
  argus::mcp::ToolOutcome result;
  switch (outcome.kind) {
    case DisableKind::Disabled:
      result.text = speech::say({.invocation = invocation,
                                 .spanish = "Listo, apagué " + card->name + ". Sus datos siguen guardados.",
                                 .english = "Done, I turned off " + card->name + ". Its data is still kept."});
      result.structured["disabled"] = true;
      co_return result;
    case DisableKind::Refused:
      co_return speech::refuse({.invocation = invocation,
                                .spanish = "No pude apagar " + card->name + ": " + outcome.detail,
                                .english = "I could not turn off " + card->name + ": " + outcome.detail},
                               "refused");
    case DisableKind::Unknown:
      co_return unknownModule(invocation);
    case DisableKind::Unavailable:
      break;
  }
  co_return unavailable(invocation);
}

drogon::Task<argus::mcp::ToolOutcome> openPurgeScreen(std::shared_ptr<ModuleDesk> desk, argus::mcp::ToolInvocation invocation)
{
  const auto card = co_await desk->find(lookupOf(invocation));
  if (!card)
    co_return unknownModule(invocation);
  if (card->state == ModuleState::ComingSoon)
    co_return speech::refuse({.invocation = invocation,
                              .spanish = card->name + " todavía no existe, no tiene datos.",
                              .english = card->name + " does not exist yet, it has no data."},
                             "coming_soon");
  argus::mcp::ToolOutcome outcome;
  outcome.text = speech::say({.invocation = invocation,
                              .spanish = "Te abrí la pantalla de datos de " + card->name +
                                         ". Desde ahí puedes borrarlos tú mismo; por voz no se borran datos.",
                              .english = "I opened the data screen of " + card->name +
                                         ". You can delete the data yourself from there; data is never deleted by voice."});
  Json::Value arguments(Json::objectValue);
  arguments["screen"] = "modules";
  arguments["module"] = card->id;
  outcome.structured = arguments;
  outcome.appAction = argus::mcp::AppAction{.name = "app.open", .arguments = std::move(arguments)};
  co_return outcome;
}

argus::mcp::ToolSpec specOf(argus::mcp::ToolSpec base)
{
  base.module = kCore;
  return base;
}

Json::Value moduleProperty()
{
  return schema::text({.description = "id del módulo: productivity, surveillance...", .minimum = std::nullopt, .maximum = std::nullopt});
}

template <class Handler>
argus::mcp::McpServer::Handler withLoop(const ModuleToolsInput& input, Handler handler)
{
  return argus::mcp::onLoop({.loop = input.loop, .handler = std::move(handler)});
}
}

std::shared_ptr<argus::mcp::McpServer> moduleToolServer(const ModuleToolsInput& input)
{
  auto server = std::make_shared<argus::mcp::McpServer>(
      argus::mcp::ServerIdentity{.name = "argus-settings", .version = "1", .instructions = ""});
  const auto desk = input.desk;
  const auto ledger = std::make_shared<argus::mcp::ConfirmationLedger>();
  server->add(specOf({.name = "modules.list",
                      .title = "",
                      .description = "Lista los módulos de Argus y si están activos, apagados o próximamente",
                      .inputSchema = schema::emptyObject(),
                      .annotations = {.readOnly = true},
                      .module = "",
                      .capability = "modules.read"}),
              withLoop(input, [desk](const argus::mcp::ToolInvocation& invocation) { return listModules(desk, invocation); }));
  server->add(specOf({.name = "modules.explain",
                      .title = "",
                      .description = "Explica qué es un módulo de Argus, con ejemplos, y si está activo",
                      .inputSchema = schema::object({{.name = "module", .schema = moduleProperty(), .required = true}}),
                      .annotations = {.readOnly = true},
                      .module = "",
                      .capability = "modules.read"}),
              withLoop(input, [desk](const argus::mcp::ToolInvocation& invocation) { return explainModule(desk, invocation); }));
  server->add(specOf({.name = "modules.request",
                      .title = "",
                      .description = "Pide al dueño de la casa que active un módulo apagado; solo con el sí del usuario",
                      .inputSchema = schema::object({{.name = "module", .schema = moduleProperty(), .required = true}}),
                      .annotations = {},
                      .module = "",
                      .capability = "modules.request"}),
              withLoop(input, [desk](const argus::mcp::ToolInvocation& invocation) { return requestModule(desk, invocation); }));
  server->add(specOf({.name = "modules.enable",
                      .title = "",
                      .description = "Activa un módulo apagado (el dueño); solo con el sí del usuario",
                      .inputSchema = schema::object({{.name = "module", .schema = moduleProperty(), .required = true}}),
                      .annotations = {.idempotent = true},
                      .module = "",
                      .capability = "modules.manage"}),
              withLoop(input, [desk](const argus::mcp::ToolInvocation& invocation) { return enableModule(desk, invocation); }));
  server->add(specOf({.name = "modules.disable",
                      .title = "",
                      .description = "Apaga un módulo (el dueño). Primero explica qué se detendría y devuelve un código; con "
                                     "el sí del usuario se repite la llamada con confirmation",
                      .inputSchema = schema::object({{.name = "module", .schema = moduleProperty(), .required = true},
                                                     {.name = "confirmation", .schema = schema::text(), .required = false}}),
                      .annotations = {.destructive = true},
                      .module = "",
                      .capability = "modules.manage"}),
              withLoop(input, [desk, ledger](const argus::mcp::ToolInvocation& invocation) {
                return disableModule(desk, ledger, invocation);
              }));
  server->add(specOf({.name = "modules.open_purge_screen",
                      .title = "",
                      .description = "Abre en la app la pantalla donde el dueño puede borrar los datos de un módulo; por voz "
                                     "nunca se borran datos",
                      .inputSchema = schema::object({{.name = "module", .schema = moduleProperty(), .required = true}}),
                      .annotations = {},
                      .module = "",
                      .capability = "modules.manage"}),
              withLoop(input, [desk](const argus::mcp::ToolInvocation& invocation) { return openPurgeScreen(desk, invocation); }));
  server->setGate(tool_gate::capabilities());
  return server;
}
