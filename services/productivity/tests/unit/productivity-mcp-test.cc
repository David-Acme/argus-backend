#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/module-gate.hxx>
#include <drogon/drogon.h>
#include <feature/mcp/services/productivity-tools.hxx>
#include <mcp/json-rpc.hxx>
#include <sqlite/db-service.hxx>
#include <sync/user-change-sink.hxx>
#include <text/iso-time.hxx>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef ARGUS_PRODUCTIVITY_SCHEMA
#error "ARGUS_PRODUCTIVITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
using namespace argus::mcp;

constexpr const char* kDb = "productivity-mcp-test.db";
constexpr int64_t kAna = 11;
constexpr int64_t kLuis = 12;

template <class T>
T must(std::optional<T> value)
{
  REQUIRE(value.has_value());
  return std::move(value).value_or(T{});
}

class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::thread runner_;
};

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

class RecordingSink final : public UserChangeSink
{
public:
  [[nodiscard]] drogon::Task<void> emitUsers(const UserEmitInput& input) const override
  {
    const std::scoped_lock lock(mutex_);
    operations.push_back(input.body.operation);
    tables.push_back(input.body.option);
    co_return;
  }

  [[nodiscard]] drogon::Task<void> publishAudit(const UserAuditInput&) const override { co_return; }

  void clear() const
  {
    const std::scoped_lock lock(mutex_);
    operations.clear();
    tables.clear();
  }

  mutable std::mutex mutex_;
  mutable std::vector<SyncOperation> operations;
  mutable std::vector<TableName> tables;
};

struct Caller
{
  int64_t userId{0};
  std::string role;
  std::string lang;
};

struct Fixture
{
  Fixture()
  {
    setenv("TZ", "UTC", 1);
    tzset();
    std::remove(kDb);
    std::remove((std::string(kDb) + "-wal").c_str());
    std::remove((std::string(kDb) + "-shm").c_str());
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{.connectionNumber = 1, .filename = kDb, .name = "default", .timeout = -1});
  }

  AppRunner runner;
};

struct Env
{
  RecordingSink sink;
  std::shared_ptr<McpServer> server = productivityToolServer({.loop = {}});

  ToolOutcome call(const std::string& tool, Json::Value arguments, const Caller& caller)
  {
    Json::Value info(Json::objectValue);
    Json::Value params(Json::objectValue);
    params["name"] = tool;
    params["arguments"] = std::move(arguments);
    params["_meta"] = requestMeta(info);
    params["_meta"]["argus/context"] = toJson(CallerContext{.userId = caller.userId,
                                                            .role = caller.role,
                                                            .lang = caller.lang,
                                                            .sessionId = "s",
                                                            .utterance = "haz algo",
                                                            .decided = false});
    const auto response = must(parseResponse(
        server->handleBlocking(requestFrame({.hasId = true, .id = Json::Value(1), .method = "tools/call", .params = params}))));
    REQUIRE(response.result.has_value());
    return must(toolOutcomeFrom(response.result.value_or(Json::Value())));
  }

  ToolOutcome ana(const std::string& tool, Json::Value arguments) { return call(tool, std::move(arguments), {.userId = kAna, .role = "resident", .lang = "es"}); }
  ToolOutcome luis(const std::string& tool, Json::Value arguments) { return call(tool, std::move(arguments), {.userId = kLuis, .role = "resident", .lang = "es"}); }
};

Json::Value args(std::initializer_list<std::pair<const char*, Json::Value>> fields)
{
  Json::Value arguments(Json::objectValue);
  for (const auto& [name, value] : fields)
    arguments[name] = value;
  return arguments;
}

int64_t count(const std::string& sql, const std::string& title)
{
  const auto rows = DbService::productivityClient()->execSqlSync(sql, title);
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}

int64_t liveEvents(const std::string& title)
{
  return count("SELECT COUNT(*) AS total FROM calendar_event WHERE title = ? AND deleted_at IS NULL", title);
}

int64_t ownerOfEvent(const std::string& title)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT owner_id FROM calendar_event WHERE title = ? AND deleted_at IS NULL", title);
  return rows.empty() ? -1 : rows.front()["owner_id"].as<int64_t>();
}

void reset(Env& env)
{
  env.sink.clear();
  moduleGate().reset();
  for (const char* table : {"calendar_event", "project_task", "project_member", "project", "idempotency_key"})
    DbService::productivityClient()->execSqlSync(std::string("DELETE FROM ") + table);
}

const char* kStart = "2030-03-06T15:00:00+00:00";

void creatingAnEventBelongsToTheCaller(Env& env)
{
  reset(env);
  const auto created = env.ana("calendar.create_event", args({{"title", "Reunión con Pedro"}, {"starts_at", kStart}, {"location", "Oficina"}}));
  CHECK_FALSE(created.isError);
  CHECK(created.text == "Quedó agendado «Reunión con Pedro» para el miércoles 6 de marzo a las 3 de la tarde.");
  CHECK(liveEvents("Reunión con Pedro") == 1);
  CHECK(ownerOfEvent("Reunión con Pedro") == kAna);
  CHECK(created.structured["title"].asString() == "Reunión con Pedro");
  CHECK(created.structured["readback"].asString() == "el miércoles 6 de marzo a las 3 de la tarde");
  REQUIRE(env.sink.operations.size() == 1);
  CHECK(env.sink.operations.front() == SyncOperation::Add);
  CHECK(env.sink.tables.front() == TableName::CalendarEvent);

  const auto again = env.ana("calendar.create_event", args({{"title", "Reunión con Pedro"}, {"starts_at", kStart}}));
  CHECK_FALSE(again.isError);
  CHECK(liveEvents("Reunión con Pedro") == 1);
  CHECK(again.structured["id"].asInt64() == created.structured["id"].asInt64());

  const auto english = env.call("calendar.create_event", args({{"title", "Dentist"}, {"starts_at", "2030-03-07T09:30:00+00:00"}}), {.userId = kAna, .role = "owner", .lang = "en"});
  CHECK(english.text == "«Dentist» is scheduled for Thursday, March 7th at 9:30 AM.");
}

void theReadBackNamesTheDayAndTheTimeInWords(Env& env)
{
  reset(env);
  const std::time_t now = std::time(nullptr);
  std::tm today{};
  localtime_r(&now, &today);
  today.tm_hour = 17;
  today.tm_min = 0;
  today.tm_sec = 0;
  today.tm_isdst = -1;
  const auto startsAt = static_cast<int64_t>(std::mktime(&today));
  const std::array<const char*, 7> days{"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
  const std::array<const char*, 7> englishDays{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  const auto weekday = static_cast<std::size_t>(today.tm_wday);
  const std::string iso = iso_time::format(startsAt);
  const auto spanish = env.ana("calendar.create_event", args({{"title", "Café"}, {"starts_at", iso}}));
  CHECK(spanish.text == std::string("Quedó agendado «Café» para el ") + days.at(weekday) + " " + std::to_string(today.tm_mday) + " a las 5 de la tarde.");
  const auto english = env.call("calendar.create_event", args({{"title", "Coffee"}, {"starts_at", iso}}), {.userId = kAna, .role = "owner", .lang = "en"});
  const int day = today.tm_mday;
  const std::string suffix = day % 10 == 1 && day != 11 ? "st" : (day % 10 == 2 && day != 12 ? "nd" : (day % 10 == 3 && day != 13 ? "rd" : "th"));
  CHECK(english.text == std::string("«Coffee» is scheduled for ") + englishDays.at(weekday) + " the " + std::to_string(day) + suffix + " at 5 PM.");
}

void anEventNeedsAUnderstandableStart(Env& env)
{
  reset(env);
  const auto missing = env.ana("calendar.create_event", args({{"title", "Sin hora"}}));
  CHECK(missing.isError);
  const auto vague = env.ana("calendar.create_event", args({{"title", "Sin hora"}, {"starts_at", "mañana"}}));
  CHECK(vague.isError);
  CHECK(vague.structured["code"].asString() == "invalid_time");
  CHECK(vague.text == "No entendí el día y la hora del evento. Dímelos otra vez.");
  const auto untitled = env.ana("calendar.create_event", args({{"title", ""}, {"starts_at", kStart}}));
  CHECK(untitled.isError);
  CHECK(untitled.structured["code"].asString() == "invalid_arguments");
  const auto reversed = env.ana("calendar.create_event", args({{"title", "Al revés"}, {"starts_at", kStart}, {"ends_at", "2030-03-06T14:00:00+00:00"}}));
  CHECK(reversed.isError);
  CHECK(liveEvents("Sin hora") + liveEvents("Al revés") == 0);
  CHECK(env.sink.operations.empty());
}

void listingShowsOnlyTheCallersEvents(Env& env)
{
  reset(env);
  env.ana("calendar.create_event", args({{"title", "Mía uno"}, {"starts_at", "2030-03-06T10:00:00+00:00"}}));
  env.ana("calendar.create_event", args({{"title", "Mía dos"}, {"starts_at", "2030-03-07T10:00:00+00:00"}}));
  env.luis("calendar.create_event", args({{"title", "De Luis"}, {"starts_at", "2030-03-06T11:00:00+00:00"}}));

  const auto listed = env.ana("calendar.list_events", args({{"from", "2030-03-06T00:00:00+00:00"}, {"to", "2030-03-08T00:00:00+00:00"}}));
  CHECK_FALSE(listed.isError);
  CHECK(listed.text.find("Tienes 2 eventos") == 0);
  CHECK(listed.text.find("Mía uno") != std::string::npos);
  CHECK(listed.text.find("Mía dos") != std::string::npos);
  CHECK(listed.text.find("De Luis") == std::string::npos);
  CHECK(listed.structured["events"].size() == 2);

  const auto limited = env.ana("calendar.list_events", args({{"from", "2030-03-06T00:00:00+00:00"}, {"to", "2030-03-08T00:00:00+00:00"}, {"limit", 1}}));
  CHECK(limited.structured["events"].size() == 1);
  CHECK(limited.text.find("Y 1 más.") != std::string::npos);

  const auto none = env.ana("calendar.list_events", args({{"from", "2031-01-01T00:00:00+00:00"}, {"to", "2031-01-02T00:00:00+00:00"}}));
  CHECK(none.text.find("No tienes eventos entre") == 0);
}

void cancellingNeedsTheConfirmationCodeAndTheOwner(Env& env)
{
  reset(env);
  env.ana("calendar.create_event", args({{"title", "Cena con Marta"}, {"starts_at", kStart}}));
  env.luis("calendar.create_event", args({{"title", "Cena con Marta"}, {"starts_at", kStart}}));
  REQUIRE(liveEvents("Cena con Marta") == 2);
  env.sink.clear();

  const auto preview = env.ana("calendar.cancel_event", args({{"title", "cena con marta"}}));
  CHECK_FALSE(preview.isError);
  CHECK(preview.structured["needsConfirmation"].asBool());
  CHECK(preview.text.find("Esto cancelaría «Cena con Marta»") == 0);
  CHECK(preview.text.find("confirmation") == std::string::npos);
  const std::string code = preview.structured["confirmation"].asString();
  REQUIRE(code.size() == 6);
  CHECK(liveEvents("Cena con Marta") == 2);
  CHECK(env.sink.operations.empty());

  const auto wrong = env.ana("calendar.cancel_event", args({{"title", "cena con marta"}, {"confirmation", "000000"}}));
  CHECK(wrong.isError);
  CHECK(wrong.structured["code"].asString() == "confirmation_invalid");
  CHECK(liveEvents("Cena con Marta") == 2);

  const auto stolen = env.luis("calendar.cancel_event", args({{"title", "cena con marta"}, {"confirmation", code}}));
  CHECK(stolen.isError);
  CHECK(liveEvents("Cena con Marta") == 2);

  const auto done = env.ana("calendar.cancel_event", args({{"title", "cena con marta"}, {"confirmation", code}}));
  CHECK_FALSE(done.isError);
  CHECK(done.text.find("Cancelado: «Cena con Marta»") == 0);
  CHECK(liveEvents("Cena con Marta") == 1);
  CHECK(ownerOfEvent("Cena con Marta") == kLuis);
  REQUIRE(env.sink.operations.size() == 1);
  CHECK(env.sink.operations.front() == SyncOperation::Delete);

  const auto reused = env.ana("calendar.cancel_event", args({{"title", "cena con marta"}, {"confirmation", code}}));
  CHECK(reused.isError);
  CHECK(liveEvents("Cena con Marta") == 1);
}

void anotherUsersEventCannotBeNamedOrCancelled(Env& env)
{
  reset(env);
  env.luis("calendar.create_event", args({{"title", "Privado de Luis"}, {"starts_at", kStart}}));
  const auto byTitle = env.ana("calendar.cancel_event", args({{"title", "Privado de Luis"}}));
  CHECK(byTitle.isError);
  CHECK(byTitle.structured["code"].asString() == "event_not_found");
  const auto rows = DbService::productivityClient()->execSqlSync("SELECT id FROM calendar_event WHERE title = 'Privado de Luis'");
  REQUIRE(rows.size() == 1);
  const auto byId = env.ana("calendar.cancel_event", args({{"event_id", rows.front()["id"].as<int64_t>()}}));
  CHECK(byId.isError);
  CHECK(byId.structured["code"].asString() == "event_not_found");
  CHECK(liveEvents("Privado de Luis") == 1);
}

void twoEventsWithTheSameWordsAskWhich(Env& env)
{
  reset(env);
  env.ana("calendar.create_event", args({{"title", "Reunión de equipo"}, {"starts_at", "2030-03-06T10:00:00+00:00"}}));
  env.ana("calendar.create_event", args({{"title", "Reunión de clientes"}, {"starts_at", "2030-03-07T10:00:00+00:00"}}));
  const auto asked = env.ana("calendar.cancel_event", args({{"title", "reunión"}}));
  CHECK(asked.isError);
  CHECK(asked.structured["code"].asString() == "ambiguous_event");
  CHECK(asked.text.find("Reunión de equipo") != std::string::npos);
  CHECK(asked.text.find("Reunión de clientes") != std::string::npos);
}

void projectsAndTasksFollowTheirProject(Env& env)
{
  reset(env);
  const auto none = env.ana("task.create", args({{"title", "Llamar al dentista"}}));
  CHECK(none.isError);
  CHECK(none.structured["code"].asString() == "no_projects");

  const auto created = env.ana("project.create", args({{"name", "Casa nueva"}, {"description", "mudanza"}}));
  CHECK_FALSE(created.isError);
  CHECK(created.text == "Proyecto creado: Casa nueva.");
  const auto project = env.ana("project.list", args({}));
  CHECK(project.text == "Tus proyectos: Casa nueva (active).");

  const auto lone = env.ana("task.create", args({{"title", "Llamar al dentista"}, {"priority", "high"}}));
  CHECK_FALSE(lone.isError);
  CHECK(lone.text == "Quedó agregada la tarea «Llamar al dentista» (Casa nueva).");
  CHECK(lone.structured["priority"].asString() == "high");

  env.ana("project.create", args({{"name", "Trabajo"}}));
  const auto needed = env.ana("task.create", args({{"title", "Enviar informe"}}));
  CHECK(needed.isError);
  CHECK(needed.structured["code"].asString() == "project_needed");
  CHECK(needed.text == "¿En cuál proyecto va? Trabajo y Casa nueva.");
  REQUIRE(needed.structured["projects"].size() == 2);
  CHECK(needed.structured["projects"][0].asString() == "Trabajo");
  CHECK(needed.structured["projects"][1].asString() == "Casa nueva");
  CHECK_FALSE(none.structured.isMember("projects"));

  const auto named = env.ana("task.create", args({{"title", "Enviar informe"}, {"project", "trabajo"}, {"due_at", "2030-03-08T00:00:00+00:00"}}));
  CHECK_FALSE(named.isError);
  CHECK(named.text == "Quedó agregada la tarea «Enviar informe» (Trabajo), para el viernes 8 de marzo.");

  const auto unknown = env.ana("task.create", args({{"title", "x"}, {"project", "jardín"}}));
  CHECK(unknown.isError);
  CHECK(unknown.structured["code"].asString() == "unknown_project");
  CHECK(unknown.text == "No encuentro un proyecto llamado jardín. Tus proyectos son: Trabajo y Casa nueva.");

  const auto pending = env.ana("task.list", args({}));
  CHECK(pending.text.find("Tareas pendientes:") == 0);
  CHECK(pending.structured["tasks"].size() == 2);
  const auto inWork = env.ana("task.list", args({{"project", "Trabajo"}}));
  CHECK(inWork.structured["tasks"].size() == 1);

  const auto done = env.ana("task.complete", args({{"title", "dentista"}}));
  CHECK_FALSE(done.isError);
  CHECK(done.text == "Hecho: «Llamar al dentista» (Casa nueva).");
  CHECK(env.ana("task.list", args({})).structured["tasks"].size() == 1);
  const auto missing = env.ana("task.complete", args({{"title", "dentista"}}));
  CHECK(missing.isError);
  CHECK(missing.structured["code"].asString() == "task_not_found");
}

void tasksOfAnotherUserAreNeverSeen(Env& env)
{
  reset(env);
  env.luis("project.create", args({{"name", "Secreto"}}));
  env.luis("task.create", args({{"title", "Plan privado"}}));
  CHECK(env.ana("project.list", args({})).text == "No tienes proyectos.");
  CHECK(env.ana("task.list", args({})).text == "No tienes tareas pendientes.");
  CHECK(env.ana("task.complete", args({{"title", "Plan privado"}})).isError);
  const auto rows = DbService::productivityClient()->execSqlSync("SELECT status FROM project_task WHERE title = 'Plan privado'");
  REQUIRE(rows.size() == 1);
  CHECK(rows.front()["status"].as<std::string>() == "todo");
}

void rolesAndModulesDecideWhoMayUseTheTools(Env& env)
{
  reset(env);
  for (const char* role : {"guest", "guard", "unknown"}) {
    const auto refused = env.call("calendar.create_event", args({{"title", "No"}, {"starts_at", kStart}}), {.userId = kAna, .role = role, .lang = "es"});
    CHECK(refused.isError);
    CHECK(refused.structured["code"].asString() == "forbidden");
  }
  CHECK(liveEvents("No") == 0);
  CHECK_FALSE(env.call("calendar.create_event", args({{"title", "Del dueño"}, {"starts_at", kStart}}), {.userId = kAna, .role = "owner", .lang = "es"}).isError);

  moduleGate().apply({{.id = "productivity", .enabled = false}});
  const auto off = env.ana("calendar.create_event", args({{"title", "Apagado"}, {"starts_at", kStart}}));
  CHECK(off.isError);
  CHECK(off.structured["code"].asString() == "module_inactive");
  CHECK(liveEvents("Apagado") == 0);
  moduleGate().reset();
}

void aCallerWithoutAnIdentityIsRefused(Env& env)
{
  reset(env);
  const auto nobody = env.call("calendar.list_events", args({}), {.userId = 0, .role = "resident", .lang = "es"});
  CHECK(nobody.isError);
  CHECK(nobody.structured["code"].asString() == "identity_required");
}

void theToolsDeclareTheirModuleAndCapability(Env& env)
{
  const std::vector<std::pair<std::string, std::string>> expected{
      {"calendar.list_events", "agenda.read"},  {"calendar.create_event", "agenda.write"},
      {"calendar.cancel_event", "agenda.write"}, {"project.list", "projects.read"},
      {"project.create", "projects.write"},     {"task.list", "projects.read"},
      {"task.create", "projects.write"},        {"task.complete", "projects.write"}};
  for (const auto& [name, capability] : expected) {
    const auto* spec = env.server->find(name);
    REQUIRE(spec != nullptr);
    CHECK(spec->module == "productivity");
    CHECK(spec->capability == capability);
  }
  CHECK(env.server->find("calendar.cancel_event")->annotations.destructive);
  CHECK(env.server->find("calendar.list_events")->annotations.readOnly);
  CHECK(env.server->find("calendar.create_event")->inputSchema["properties"]["starts_at"]["format"].asString() == "date-time");
}
}

TEST_CASE("the productivity tools act for the caller on the caller's own rows")
{
  Fixture fixture;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_PRODUCTIVITY_SCHEMA));

  Env env;
  user_change::setProductivitySink(&env.sink);
  theToolsDeclareTheirModuleAndCapability(env);
  creatingAnEventBelongsToTheCaller(env);
  theReadBackNamesTheDayAndTheTimeInWords(env);
  anEventNeedsAUnderstandableStart(env);
  listingShowsOnlyTheCallersEvents(env);
  cancellingNeedsTheConfirmationCodeAndTheOwner(env);
  anotherUsersEventCannotBeNamedOrCancelled(env);
  twoEventsWithTheSameWordsAskWhich(env);
  projectsAndTasksFollowTheirProject(env);
  tasksOfAnotherUserAreNeverSeen(env);
  rolesAndModulesDecideWhoMayUseTheTools(env);
  aCallerWithoutAnIdentityIsRefused(env);
  user_change::setProductivitySink(nullptr);
}
