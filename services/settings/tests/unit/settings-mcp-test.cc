#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/mcp/services/module-tools.hxx>
#include <mcp/json-rpc.hxx>

#include <trantor/net/EventLoopThread.h>

#include <map>
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

class FakeDesk final : public ModuleDesk
{
public:
  FakeDesk()
  {
    cards = {{.id = "core", .name = "Núcleo", .summary = "Lo básico", .what = "Lo básico de Argus.", .examples = {}, .state = ModuleState::Active},
             {.id = "productivity",
              .name = "Productividad",
              .summary = "Agenda",
              .what = "Organiza tu agenda, proyectos y tareas.",
              .examples = {"agenda una reunión", "crea una tarea"},
              .state = ModuleState::Off},
             {.id = "surveillance",
              .name = "Vigilancia",
              .summary = "Cámaras",
              .what = "Cuida la casa con cámaras.",
              .examples = {},
              .state = ModuleState::Active},
             {.id = "agronomy", .name = "Agronomía", .summary = "Cultivos", .what = "", .examples = {}, .state = ModuleState::ComingSoon}};
  }

  drogon::Task<std::vector<ModuleCard>> list(const std::string&) override { co_return cards; }

  drogon::Task<std::optional<ModuleCard>> find(const DeskLookup& lookup) override
  {
    for (const auto& card : cards)
      if (card.id == lookup.moduleId)
        co_return card;
    co_return std::nullopt;
  }

  drogon::Task<EnableOutcome> enable(const DeskCommand& command) override
  {
    enabled.push_back(command.moduleId);
    co_return enableAnswer;
  }

  drogon::Task<ModuleImpact> impact(const DeskLookup&) override { co_return impactAnswer; }

  drogon::Task<DisableOutcome> disable(const DeskCommand& command) override
  {
    disabled.push_back(command.moduleId);
    co_return disableAnswer;
  }

  drogon::Task<RequestOutcome> request(const DeskCommand& command) override
  {
    requested.emplace_back(command.userId, command.moduleId);
    requestedRoles.push_back(command.role);
    co_return requestAnswer;
  }

  std::vector<ModuleCard> cards;
  EnableOutcome enableAnswer{.kind = EnableKind::Started, .detail = ""};
  DisableOutcome disableAnswer{.kind = DisableKind::Disabled, .detail = ""};
  RequestOutcome requestAnswer{.kind = RequestKind::Requested};
  ModuleImpact impactAnswer{.known = true,
                            .allowed = true,
                            .refusal = "",
                            .refusalCode = "",
                            .stops = {{.kind = "live_views", .count = std::nullopt},
                                      {.kind = "guard_duty", .count = 2},
                                      {.kind = "something_new", .count = std::nullopt}},
                            .holders = {{.name = "Ana", .role = "guard"}},
                            .invitations = {{.role = "guard", .invitedBy = "Luis"}},
                            .keepsRunning = {},
                            .unreachable = {}};
  std::vector<std::string> enabled;
  std::vector<std::string> disabled;
  std::vector<std::pair<int64_t, std::string>> requested;
  std::vector<std::string> requestedRoles;
};

struct Harness
{
  trantor::EventLoopThread thread;
  std::shared_ptr<FakeDesk> desk = std::make_shared<FakeDesk>();
  std::shared_ptr<McpServer> server;

  Harness()
  {
    thread.run();
    trantor::EventLoop* loop = thread.getLoop();
    server = moduleToolServer({.desk = desk, .loop = [loop] { return loop; }});
  }

  ToolOutcome call(const std::string& tool, Json::Value arguments, const std::string& role, const std::string& lang = "es", int64_t userId = 9)
  {
    Json::Value info(Json::objectValue);
    Json::Value params(Json::objectValue);
    params["name"] = tool;
    params["arguments"] = std::move(arguments);
    params["_meta"] = requestMeta(info);
    params["_meta"]["argus/context"] =
        toJson(CallerContext{.userId = userId, .role = role, .lang = lang, .sessionId = "s", .utterance = "x", .decided = false});
    const auto response = must(parseResponse(
        server->handleBlocking(requestFrame({.hasId = true, .id = Json::Value(1), .method = "tools/call", .params = params}))));
    REQUIRE(response.result.has_value());
    return must(toolOutcomeFrom(response.result.value_or(Json::Value())));
  }
};

struct ModuleArguments
{
  std::string id;
  std::string confirmation;
};

Json::Value confirmed(const ModuleArguments& input)
{
  Json::Value arguments(Json::objectValue);
  arguments["module"] = input.id;
  if (!input.confirmation.empty())
    arguments["confirmation"] = input.confirmation;
  return arguments;
}

Json::Value module(const std::string& id)
{
  return confirmed({.id = id, .confirmation = ""});
}

std::string codeIn(const argus::mcp::ToolOutcome& preview)
{
  const std::string code = preview.structured["confirmation"].asString();
  REQUIRE(code.size() == 6);
  CHECK(preview.text.find("confirmation") == std::string::npos);
  return code;
}

struct DisableExpectation
{
  const char* lang;
  const char* previewPrefix;
  const char* holders;
  const char* invitations;
  const char* nothingDeleted;
  const char* done;
};

constexpr DisableExpectation kSpanishDisable{
    .lang = "es",
    .previewPrefix = "Apagar Vigilancia detendría las imágenes en vivo, las guardias en curso (2) y something new.",
    .holders = "Su rol quedaría inactivo para: Ana (guard).",
    .invitations = "Invitaciones pendientes que se revocarían: 1.",
    .nothingDeleted = "No se borra nada",
    .done = "Listo, apagué Vigilancia. Sus datos siguen guardados."};

constexpr DisableExpectation kEnglishDisable{
    .lang = "en",
    .previewPrefix = "Turning off Vigilancia would stop live views, guard duty in progress (2) and something new.",
    .holders = "Their role would become inactive for: Ana (guard).",
    .invitations = "Pending invitations that would be revoked: 1.",
    .nothingDeleted = "Nothing is deleted",
    .done = "Done, I turned off Vigilancia. Its data is still kept."};

void disablingPreviewsAndNeedsTheOneUseCode(const DisableExpectation& expected)
{
  Harness harness;
  const auto preview = harness.call("modules.disable", module("surveillance"), "owner", expected.lang, 1);
  CHECK_FALSE(preview.isError);
  CHECK(preview.structured["needsConfirmation"].asBool());
  CHECK(preview.text.find(expected.previewPrefix) == 0);
  CHECK(preview.text.find(expected.holders) != std::string::npos);
  CHECK(preview.text.find(expected.invitations) != std::string::npos);
  CHECK(preview.text.find(expected.nothingDeleted) != std::string::npos);
  CHECK(harness.desk->disabled.empty());
  const std::string code = codeIn(preview);

  const auto wrong = harness.call("modules.disable", confirmed({.id = "surveillance", .confirmation = "000000"}), "owner", expected.lang, 1);
  CHECK(wrong.isError);
  CHECK(wrong.structured["code"].asString() == "confirmation_invalid");
  CHECK(harness.desk->disabled.empty());

  const auto elsewhere = harness.call("modules.disable", confirmed({.id = "productivity", .confirmation = code}), "owner", expected.lang, 1);
  CHECK(elsewhere.isError);

  const auto done = harness.call("modules.disable", confirmed({.id = "surveillance", .confirmation = code}), "owner", expected.lang, 1);
  CHECK_FALSE(done.isError);
  CHECK(done.text == expected.done);
  CHECK(harness.desk->disabled == std::vector<std::string>{"surveillance"});

  const auto again = harness.call("modules.disable", confirmed({.id = "surveillance", .confirmation = code}), "owner", expected.lang, 1);
  CHECK(again.isError);
  CHECK(harness.desk->disabled.size() == 1);
}
}

TEST_CASE("every module tool is core, names the capability the access table knows and says if it destroys")
{
  Harness harness;
  const std::vector<std::pair<std::string, std::string>> expected{
      {"modules.list", "modules.read"},     {"modules.explain", "modules.read"},
      {"modules.request", "modules.request"}, {"modules.enable", "modules.manage"},
      {"modules.disable", "modules.manage"}, {"modules.open_purge_screen", "modules.manage"}};
  for (const auto& [name, capability] : expected) {
    const auto* spec = harness.server->find(name);
    REQUIRE(spec != nullptr);
    CHECK(spec->module == "core");
    CHECK(spec->capability == capability);
  }
  CHECK(harness.server->find("modules.disable")->annotations.destructive);
  CHECK_FALSE(harness.server->find("modules.enable")->annotations.destructive);
  CHECK(harness.server->tools().size() == expected.size());
}

TEST_CASE("every role lists and explains the modules in its own language")
{
  Harness harness;
  for (const char* role : {"owner", "resident", "guard", "guest"}) {
    const auto listed = harness.call("modules.list", Json::Value(Json::objectValue), role);
    CHECK_FALSE(listed.isError);
    CHECK(listed.text == "Los módulos de Argus: Núcleo (activo), Productividad (apagado), Vigilancia (activo) y Agronomía (próximamente).");
  }
  const auto english = harness.call("modules.list", Json::Value(Json::objectValue), "guest", "en");
  CHECK(english.text == "The Argus modules: Núcleo (on), Productividad (off), Vigilancia (on) and Agronomía (coming soon).");

  const auto explained = harness.call("modules.explain", module("productivity"), "resident");
  CHECK(explained.text == "Productividad: Organiza tu agenda, proyectos y tareas. Por ejemplo: agenda una reunión y crea una tarea. Ahora está apagado.");
  CHECK_FALSE(harness.call("modules.explain", module("nope"), "resident").text.empty());
  CHECK(harness.call("modules.explain", module("nope"), "resident").structured["code"].asString() == "unknown_module");
}

TEST_CASE("an installing module says how far it is")
{
  Harness harness;
  harness.desk->cards[1].state = ModuleState::Installing;
  harness.desk->cards[1].progress = 0.42;
  CHECK(harness.call("modules.explain", module("productivity"), "owner").text.find("Ahora está instalándose 42%.") != std::string::npos);
}

TEST_CASE("a member who is not the owner asks the owner and the owner cannot ask itself")
{
  Harness harness;
  const auto asked = harness.call("modules.request", module("productivity"), "resident", "es", 7);
  CHECK_FALSE(asked.isError);
  CHECK(asked.text == "Le avisé al dueño de la casa para que active Productividad.");
  REQUIRE(harness.desk->requested.size() == 1);
  CHECK(harness.desk->requested.front() == std::make_pair(int64_t{7}, std::string("productivity")));

  harness.desk->requestAnswer.kind = RequestKind::Duplicate;
  CHECK(harness.call("modules.request", module("productivity"), "guest").text == "Ya le había avisado hoy al dueño sobre Productividad.");
  harness.desk->requestAnswer.kind = RequestKind::AlreadyActive;
  CHECK(harness.call("modules.request", module("productivity"), "guard").structured["code"].asString() == "already_active");
  harness.desk->requestAnswer.kind = RequestKind::ComingSoon;
  CHECK(harness.call("modules.request", module("agronomy"), "guard").structured["code"].asString() == "coming_soon");

  const size_t before = harness.desk->requested.size();
  const auto owner = harness.call("modules.request", module("productivity"), "owner");
  CHECK(owner.isError);
  CHECK(owner.structured["code"].asString() == "forbidden");
  CHECK(harness.desk->requested.size() == before);
}

TEST_CASE("only the owner enables a module and every answer is spoken")
{
  Harness harness;
  for (const char* role : {"resident", "guard", "guest", "unknown"}) {
    const auto refused = harness.call("modules.enable", module("productivity"), role);
    CHECK(refused.isError);
    CHECK(refused.structured["code"].asString() == "forbidden");
  }
  CHECK(harness.desk->enabled.empty());

  const auto started = harness.call("modules.enable", module("productivity"), "owner");
  CHECK_FALSE(started.isError);
  CHECK(started.text == "Estoy activando Productividad; te aviso cuando esté lista.");
  CHECK(harness.desk->enabled == std::vector<std::string>{"productivity"});

  const std::vector<std::pair<EnableKind, std::string>> refusals{{EnableKind::ComingSoon, "coming_soon"},
                                                               {EnableKind::HardwareInsufficient, "hardware_insufficient"},
                                                               {EnableKind::JobRunning, "job_running"},
                                                               {EnableKind::Unknown, "unknown_module"},
                                                               {EnableKind::Unavailable, "unavailable"}};
  for (const auto& [kind, code] : refusals) {
    harness.desk->enableAnswer.kind = kind;
    const auto outcome = harness.call("modules.enable", module("productivity"), "owner");
    CHECK(outcome.isError);
    CHECK(outcome.structured["code"].asString() == code);
  }
  harness.desk->enableAnswer = {.kind = EnableKind::HardwareInsufficient, .detail = "faltan 4 GB de memoria"};
  CHECK(harness.call("modules.enable", module("surveillance"), "owner").text == "Este equipo no alcanza para Vigilancia: faltan 4 GB de memoria.");
  harness.desk->enableAnswer = {.kind = EnableKind::AlreadyActive, .detail = ""};
  const auto already = harness.call("modules.enable", module("surveillance"), "owner");
  CHECK_FALSE(already.isError);
  CHECK(already.text == "Vigilancia ya está activo.");
}

TEST_CASE("turning a module off previews what stops and who is affected, then needs the one-use code")
{
  disablingPreviewsAndNeedsTheOneUseCode(kSpanishDisable);
  disablingPreviewsAndNeedsTheOneUseCode(kEnglishDisable);
}

TEST_CASE("another person cannot spend the owner's code, and the preview is spoken in English on request")
{
  Harness harness;
  const auto preview = harness.call("modules.disable", module("surveillance"), "owner", "en", 1);
  CHECK(preview.text.find("Turning off Vigilancia would stop live views, guard duty in progress (2) and something new.") == 0);
  const std::string code = codeIn(preview);
  const auto other = harness.call("modules.disable", confirmed({.id = "surveillance", .confirmation = code}), "owner", "en", 2);
  CHECK(other.isError);
  CHECK(harness.desk->disabled.empty());
}

TEST_CASE("the preview speaks what keeps running, from the impact and in the user's language, before asking to confirm")
{
  Harness harness;
  harness.desk->impactAnswer.keepsRunning = {
      {.spanish = "las alertas de pánico o coacción en curso seguirán hasta que alguien las atienda",
       .english = "panic or duress alerts already raised keep going until someone answers them"}};

  const auto spanish = harness.call("modules.disable", module("surveillance"), "owner", "es", 1);
  CHECK_FALSE(spanish.isError);
  const auto keeps = spanish.text.find("las alertas de pánico o coacción en curso seguirán hasta que alguien las atienda.");
  REQUIRE(keeps != std::string::npos);
  CHECK(keeps < spanish.text.find("No se borra nada"));
  CHECK(spanish.structured["needsConfirmation"].asBool());

  const auto english = harness.call("modules.disable", module("surveillance"), "owner", "en", 1);
  const auto keepsEnglish = english.text.find("panic or duress alerts already raised keep going until someone answers them.");
  REQUIRE(keepsEnglish != std::string::npos);
  CHECK(english.text.find("pánico") == std::string::npos);
}

TEST_CASE("a note with one language is spoken in both, a blank one is skipped and none adds nothing")
{
  Harness harness;
  harness.desk->impactAnswer.keepsRunning = {{.spanish = "", .english = ""}, {.spanish = "las alarmas activas siguen.", .english = ""}};
  const auto spanish = harness.call("modules.disable", module("surveillance"), "owner", "es", 1);
  CHECK(spanish.text.find("las alarmas activas siguen.") != std::string::npos);
  CHECK(spanish.text.find("siguen..") == std::string::npos);
  const auto english = harness.call("modules.disable", module("surveillance"), "owner", "en", 1);
  CHECK(english.text.find("las alarmas activas siguen.") != std::string::npos);

  harness.desk->impactAnswer.keepsRunning = {};
  const auto bare = harness.call("modules.disable", module("surveillance"), "owner", "es", 1);
  CHECK(bare.text.find("seguirán") == std::string::npos);
}

TEST_CASE("an owner that did not answer is named and the list is called incomplete")
{
  Harness harness;
  harness.desk->impactAnswer.unreachable = {"cámaras", "guardia"};
  const auto spanish = harness.call("modules.disable", module("surveillance"), "owner", "es", 1);
  CHECK(spanish.text.find("No pude consultar a cámaras y guardia, así que esta lista puede estar incompleta.") != std::string::npos);
  const auto english = harness.call("modules.disable", module("surveillance"), "owner", "en", 1);
  CHECK(english.text.find("I could not reach cámaras and guardia, so this list may be incomplete.") != std::string::npos);
  CHECK(english.structured["needsConfirmation"].asBool());
}

TEST_CASE("a huge impact is said in a bounded preview: eight of each, the rest counted, long names clipped")
{
  Harness harness;
  ModuleImpact huge{.known = true,
                    .allowed = true,
                    .refusal = "",
                    .refusalCode = "",
                    .stops = {},
                    .holders = {},
                    .invitations = {},
                    .keepsRunning = {},
                    .unreachable = {}};
  for (int index = 0; index < 500; ++index)
    huge.stops.push_back({.kind = "effect_" + std::to_string(index), .count = std::nullopt});
  for (int index = 0; index < 1000; ++index) {
    huge.holders.push_back({.name = index == 0 ? std::string(100000, 'a') : "Persona " + std::to_string(index), .role = "guard"});
    huge.invitations.push_back({.role = "guard", .invitedBy = "Luis"});
  }
  for (int index = 0; index < 300; ++index) {
    huge.unreachable.push_back("dueño" + std::to_string(index));
    huge.keepsRunning.push_back({.spanish = "nota " + std::to_string(index) + std::string(5000, 'x'), .english = ""});
  }
  harness.desk->impactAnswer = huge;

  const auto spanish = harness.call("modules.disable", module("surveillance"), "owner", "es", 1);
  CHECK_FALSE(spanish.isError);
  CHECK(spanish.text.size() < 6000);
  CHECK(spanish.text.find("492 más") != std::string::npos);
  CHECK(spanish.text.find("992 más") != std::string::npos);
  CHECK(spanish.text.find("292 más") != std::string::npos);
  CHECK(spanish.text.find("…") != std::string::npos);
  CHECK(spanish.text.find("nota 7") != std::string::npos);
  CHECK(spanish.text.find("nota 8") == std::string::npos);
  CHECK(spanish.text.find("Invitaciones pendientes que se revocarían: 1000.") != std::string::npos);
  CHECK(spanish.structured["needsConfirmation"].asBool());

  const auto english = harness.call("modules.disable", module("surveillance"), "owner", "en", 1);
  CHECK(english.text.size() < 6000);
  CHECK(english.text.find("492 more") != std::string::npos);
}

TEST_CASE("a request that finds the module being installed says so and carries the caller's role")
{
  Harness harness;
  harness.desk->requestAnswer = {.kind = RequestKind::Installing};
  const auto outcome = harness.call("modules.request", module("productivity"), "guest", "es", 7);
  CHECK(outcome.isError);
  CHECK(outcome.text == "Productividad ya se está instalando.");
  CHECK(outcome.structured["code"].asString() == "job_running");
  REQUIRE(harness.desk->requestedRoles.size() == 1);
  CHECK(harness.desk->requestedRoles.front() == "guest");
}

TEST_CASE("a module that cannot be turned off says why and issues no code")
{
  Harness harness;
  harness.desk->impactAnswer = {.known = true, .allowed = false, .refusal = "otro módulo lo necesita", .refusalCode = "MODULE_REQUIRED_BY", .stops = {}, .holders = {}, .invitations = {}, .keepsRunning = {}, .unreachable = {}};
  const auto refused = harness.call("modules.disable", module("core"), "owner");
  CHECK(refused.isError);
  CHECK(refused.text == "No se puede apagar Núcleo: otro módulo lo necesita");
  CHECK(refused.structured["code"].asString() == "MODULE_REQUIRED_BY");
  CHECK(refused.text.find("confirmation") == std::string::npos);
  CHECK_FALSE(refused.structured.isMember("confirmation"));
}

TEST_CASE("a failed disable after a valid code is reported and the module is not claimed off")
{
  Harness harness;
  const std::string code = codeIn(harness.call("modules.disable", module("surveillance"), "owner"));
  harness.desk->disableAnswer = {.kind = DisableKind::Refused, .detail = "hay un trabajo en curso"};
  const auto outcome = harness.call("modules.disable", confirmed({.id = "surveillance", .confirmation = code}), "owner");
  CHECK(outcome.isError);
  CHECK(outcome.text == "No pude apagar Vigilancia: hay un trabajo en curso");
}

TEST_CASE("the purge screen is opened for the owner and nothing is ever deleted by the tool")
{
  Harness harness;
  const auto opened = harness.call("modules.open_purge_screen", module("surveillance"), "owner");
  CHECK_FALSE(opened.isError);
  CHECK(opened.text.find("por voz no se borran datos") != std::string::npos);
  const auto action = must(opened.appAction);
  CHECK(action.name == "app.open");
  CHECK(action.arguments["screen"].asString() == "modules");
  CHECK(action.arguments["module"].asString() == "surveillance");
  CHECK(harness.desk->disabled.empty());
  CHECK(harness.desk->enabled.empty());

  CHECK(harness.call("modules.open_purge_screen", module("agronomy"), "owner").structured["code"].asString() == "coming_soon");
  CHECK(harness.call("modules.open_purge_screen", module("surveillance"), "resident").structured["code"].asString() == "forbidden");
}
