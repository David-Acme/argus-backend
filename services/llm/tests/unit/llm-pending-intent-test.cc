#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tool-stubs.hxx"

#include <auth/module-snapshot.hxx>
#include <config/config-service.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/pending-intent/services/module-intent-feed.hxx>
#include <feature/pending-intent/services/pending-intent-service.hxx>
#include <nats/nats-subject.hxx>
#include <text/iso-time.hxx>
#include <text/json-util.hxx>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <optional>
#include <set>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
#ifndef ARGUS_TEST_MEMORY_SCHEMA
#define ARGUS_TEST_MEMORY_SCHEMA "services/llm/database/schema.sql"
#endif

constexpr const char* kScratchConfig = "llm-pending-intent-test.toml";
constexpr const char* kScratchDir = "/tmp/pending-intent-test";
constexpr int64_t kOwner = 3;
constexpr int64_t kResident = 4;

class Notices final : public IntentNotifier
{
public:
  [[nodiscard]] bool tell(const IntentNotice& notice) const override
  {
    const std::scoped_lock lock(mutex);
    told.push_back(notice);
    return delivers;
  }

  [[nodiscard]] size_t count() const
  {
    const std::scoped_lock lock(mutex);
    return told.size();
  }

  mutable std::mutex mutex;
  mutable std::vector<IntentNotice> told;
  bool delivers{true};
};

class Rows final : public ReminderRowWriter
{
public:
  [[nodiscard]] bool create(const ReminderRowRequest& request) const override
  {
    created.push_back(request);
    return accepts;
  }

  [[nodiscard]] std::optional<std::vector<ReminderRowInfo>> list(const ReminderRowQuery&) const override { return std::nullopt; }

  bool accepts{true};
  mutable std::vector<ReminderRowRequest> created;
};

struct Run
{
  tools::ToolCall call;
  UserRole role{UserRole::Unknown};
};

struct World
{
  SqliteGraph graph;
  std::shared_ptr<Notices> notices = std::make_shared<Notices>();
  std::shared_ptr<Rows> rows = std::make_shared<Rows>();
  std::vector<Run> runs;
  tools::ToolResult answer;
  std::set<std::string> active;
  std::atomic<bool> offline{false};
  int64_t now{1900000000};
  std::shared_ptr<PendingIntentService> service;

  explicit World(const std::string& database)
  {
    std::filesystem::create_directories(kScratchDir);
    std::remove(kScratchConfig);
    {
      std::ofstream config(kScratchConfig);
      config << "[database]\nfile = \"" << kScratchDir << "/" << database << "\"\n"
             << "[memory]\nschema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
             << "catalog_person_table = \"catalog_person\"\ncatalog_camera_table = \"catalog_camera\"\n"
             << "catalog_zone_table = \"catalog_zone\"\ncatalog_stream_table = \"catalog_stream\"\ncreate_face_vec = false\n";
    }
    ConfigService::load(kScratchConfig);
    std::filesystem::remove(std::string(kScratchDir) + "/" + database);
    REQUIRE(graph.open(std::string(kScratchDir) + "/" + database));
    graph.applySchema();
    answer.ok = true;
    answer.output = "Agendado: «Reunión con Pedro».";
    service = makeService();
  }

  std::shared_ptr<PendingIntentService> makeService()
  {
    return std::make_shared<PendingIntentService>(
        PendingIntentDependencies{.graph = &graph,
                                  .run = [this](const tools::ToolCall& call, UserRole role) {
                                    runs.push_back({.call = call, .role = role});
                                    if (offline.load()) {
                                      tools::ToolResult refused;
                                      refused.ok = false;
                                      refused.code = "module_inactive";
                                      return refused;
                                    }
                                    return answer;
                                  },
                                  .notifier = notices,
                                  .reminders = rows,
                                  .moduleName = [](const ModuleLabel& label) {
                                    return label.module == "productivity" ? (label.lang == "en" ? std::string("Productivity") : std::string("Productividad"))
                                                                          : label.module;
                                  },
                                  .moduleActive = [this](const std::string& module) { return active.contains(module); },
                                  .clock = [this] { return now; }},
        PendingIntentLimits{.offerTtlS = 3600, .waitTtlS = 86400, .keepSettledS = int64_t{86400} * 30, .sweepEvery = std::chrono::milliseconds(20)});
  }

  [[nodiscard]] tools::IntentOffer offerFor(int64_t user, const std::string& module = "productivity") const
  {
    Json::Value arguments(Json::objectValue);
    arguments["title"] = "Reunión con Pedro";
    arguments["starts_at"] = iso_time::format(now + 90000);
    return {.userId = user,
            .role = user == kOwner ? UserRole::Owner : UserRole::Resident,
            .module = module,
            .tool = "calendar.create_event",
            .arguments = arguments,
            .lang = "es",
            .utterance = "agéndame una reunión con Pedro mañana a las tres",
            .sessionId = "voice-1"};
  }

  std::string stateOf(int64_t id)
  {
    const std::scoped_lock lock(graph.mutex());
    const auto row = PendingIntentRepository{}.find(graph.handle(), id);
    return row ? std::string(pendingIntentStateToString(row->state)) : std::string("missing");
  }

  std::vector<PendingIntentRow> waiting(const std::string& module = "")
  {
    const std::scoped_lock lock(graph.mutex());
    return PendingIntentRepository{}.waiting(graph.handle(), module);
  }

  int64_t lastId()
  {
    const std::scoped_lock lock(graph.mutex());
    return sqlite3_last_insert_rowid(graph.handle());
  }
};

template <class T>
T must(std::optional<T> value)
{
  REQUIRE(value.has_value());
  return std::move(value).value_or(T{});
}

PendingIntentRow rowOf(World& world, int64_t id)
{
  const std::scoped_lock lock(world.graph.mutex());
  return must(PendingIntentRepository{}.find(world.graph.handle(), id));
}

bool waitFor(const std::function<bool()>& condition)
{
  for (int attempt = 0; attempt < 500 && !condition(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return condition();
}
}

TEST_CASE("an offer is kept with what the user asked, and a newer offer for the same module replaces it")
{
  World world("offers.db");
  world.service->offered(world.offerFor(kOwner));
  const int64_t first = world.lastId();
  CHECK(world.stateOf(first) == "offered");
  const auto row = rowOf(world, first);
  CHECK(row.userId == kOwner);
  CHECK(row.role == "owner");
  CHECK(row.module == "productivity");
  CHECK(row.tool == "calendar.create_event");
  CHECK(row.lang == "es");
  CHECK(row.sessionId == "voice-1");
  CHECK(row.utterance.find("reunión con Pedro") != std::string::npos);
  CHECK(json_util::fromString(row.arguments)["title"].asString() == "Reunión con Pedro");

  world.now += 5;
  world.service->offered(world.offerFor(kOwner));
  const int64_t second = world.lastId();
  CHECK(second != first);
  CHECK(world.stateOf(first) == "expired");
  CHECK(world.stateOf(second) == "offered");

  world.service->offered(world.offerFor(kResident));
  CHECK(world.stateOf(second) == "offered");
}

TEST_CASE("a request nobody identified is not kept")
{
  World world("anonymous.db");
  world.service->offered(world.offerFor(0));
  const std::scoped_lock lock(world.graph.mutex());
  CHECK(PendingIntentRepository{}.waiting(world.graph.handle(), "").empty());
  CHECK_FALSE(PendingIntentRepository{}.find(world.graph.handle(), 1).has_value());
}

TEST_CASE("accepting turns the user's offer for that module into a waiting intent and nobody else's")
{
  World world("accept.db");
  world.service->offered(world.offerFor(kOwner));
  const int64_t owners = world.lastId();
  world.service->offered(world.offerFor(kResident));
  const int64_t residents = world.lastId();
  world.service->offered(world.offerFor(kOwner, "surveillance"));
  const int64_t other = world.lastId();

  world.service->accepted({.userId = kOwner, .module = "productivity"});
  CHECK(world.stateOf(owners) == "waiting");
  CHECK(world.stateOf(residents) == "offered");
  CHECK(world.stateOf(other) == "offered");
  CHECK(world.waiting("productivity").size() == 1);
  CHECK(world.waiting().size() == 1);
}

TEST_CASE("when the module becomes active the stored call runs as the user who asked, the user is told and it never runs twice")
{
  World world("activate.db");
  world.service->offered(world.offerFor(kOwner));
  const int64_t id = world.lastId();
  world.service->accepted({.userId = kOwner, .module = "productivity"});

  world.service->moduleActivated("productivity");
  REQUIRE(world.runs.size() == 1);
  const Run& run = world.runs.front();
  CHECK(run.call.name == "calendar.create_event");
  CHECK(run.call.arguments["title"].asString() == "Reunión con Pedro");
  CHECK(run.call.context.userId == kOwner);
  CHECK(run.call.context.lang == "es");
  CHECK(run.call.context.sessionId == "voice-1");
  CHECK(run.call.context.utterance == "agéndame una reunión con Pedro mañana a las tres");
  CHECK(run.call.context.decided);
  CHECK(run.role == UserRole::Owner);
  CHECK(world.stateOf(id) == "done");

  REQUIRE(world.notices->count() == 1);
  const IntentNotice& notice = world.notices->told.front();
  CHECK(notice.userId == kOwner);
  CHECK(notice.title == "Tu petición está lista");
  CHECK(notice.body == "Activé Productividad y la completé. Agendado: «Reunión con Pedro».");
  CHECK(notice.commandId == "intent-" + std::to_string(id) + "-done");
  CHECK(world.rows->created.empty());

  world.service->moduleActivated("productivity");
  CHECK(world.runs.size() == 1);
  CHECK(world.notices->count() == 1);
}

TEST_CASE("the notice is spoken in the language the user asked in")
{
  World world("english.db");
  auto offer = world.offerFor(kOwner);
  offer.lang = "en";
  world.service->offered(offer);
  world.service->accepted({.userId = kOwner, .module = "productivity"});
  world.answer.output = "Scheduled: «Meeting with Pedro».";
  world.service->moduleActivated("productivity");
  REQUIRE(world.notices->count() == 1);
  CHECK(world.notices->told.front().title == "Your request is done");
  CHECK(world.notices->told.front().body == "I turned on Productivity and completed it. Scheduled: «Meeting with Pedro».");
}

TEST_CASE("a module that is still off leaves the intent waiting and says nothing")
{
  World world("still-off.db");
  world.service->offered(world.offerFor(kOwner));
  const int64_t id = world.lastId();
  world.service->accepted({.userId = kOwner, .module = "productivity"});
  world.answer.ok = false;
  world.answer.code = "module_inactive";
  world.service->moduleActivated("productivity");
  CHECK(world.stateOf(id) == "waiting");
  CHECK(world.notices->count() == 0);
  CHECK(world.rows->created.empty());
}

TEST_CASE("a stored call that fails once the module is on becomes a reminder and the user is told so")
{
  World world("run-fails.db");
  world.service->offered(world.offerFor(kOwner));
  const int64_t id = world.lastId();
  world.service->accepted({.userId = kOwner, .module = "productivity"});
  world.answer.ok = false;
  world.answer.code = "invalid_time";
  world.answer.output = "No entendí la hora";
  world.service->moduleActivated("productivity");
  CHECK(world.stateOf(id) == "failed");
  REQUIRE(world.rows->created.size() == 1);
  const auto& reminder = world.rows->created.front();
  CHECK(reminder.userId == kOwner);
  CHECK(reminder.role == "owner");
  CHECK(reminder.title == "Reunión con Pedro");
  CHECK(reminder.scheduledAt == iso_time::parse(iso_time::format(world.now + 90000)).value_or(0));
  CHECK(reminder.commandId == "intent-" + std::to_string(id));
  REQUIRE(world.notices->count() == 1);
  CHECK(world.notices->told.front().title == "No pude completar tu petición");
  CHECK(world.notices->told.front().body.find("Dejé tu petición como recordatorio: «Reunión con Pedro».") != std::string::npos);
}

TEST_CASE("a failed installation saves the request as the user's own reminder and says why")
{
  World world("install-fails.db");
  world.service->offered(world.offerFor(kOwner));
  world.service->accepted({.userId = kOwner, .module = "productivity"});
  world.service->offered(world.offerFor(kResident));
  world.service->accepted({.userId = kResident, .module = "productivity"});
  world.service->offered(world.offerFor(kOwner, "surveillance"));
  world.service->accepted({.userId = kOwner, .module = "surveillance"});

  world.service->moduleFailed({.module = "productivity", .reason = "disk_full"});
  REQUIRE(world.rows->created.size() == 2);
  CHECK(world.rows->created[0].userId == kOwner);
  CHECK(world.rows->created[1].userId == kResident);
  CHECK(world.rows->created[1].role == "resident");
  REQUIRE(world.notices->count() == 2);
  CHECK(world.notices->told[0].body.find("No pude activar Productividad (no hay espacio en el disco).") == 0);
  CHECK(world.notices->told[0].commandId.ends_with("-failed"));
  CHECK(world.runs.empty());
  CHECK(world.waiting("surveillance").size() == 1);
  CHECK(world.waiting("productivity").empty());
}

TEST_CASE("a reminder for a request without a future time is due in a minute, and without a writer the user is told it was not saved")
{
  World world("no-time.db");
  auto offer = world.offerFor(kOwner);
  offer.arguments = Json::Value(Json::objectValue);
  offer.arguments["name"] = "Casa nueva";
  offer.tool = "project.create";
  world.service->offered(offer);
  world.service->accepted({.userId = kOwner, .module = "productivity"});
  world.service->moduleFailed({.module = "productivity", .reason = "network"});
  REQUIRE(world.rows->created.size() == 1);
  CHECK(world.rows->created.front().title == "Casa nueva");
  CHECK(world.rows->created.front().scheduledAt == world.now + 60);

  World bare("no-writer.db");
  bare.service = std::make_shared<PendingIntentService>(
      PendingIntentDependencies{.graph = &bare.graph,
                                .run = {},
                                .notifier = bare.notices,
                                .reminders = nullptr,
                                .moduleName = {},
                                .moduleActive = {},
                                .clock = [&bare] { return bare.now; }},
      PendingIntentLimits{});
  bare.service->offered(bare.offerFor(kOwner));
  bare.service->accepted({.userId = kOwner, .module = "productivity"});
  bare.service->moduleFailed({.module = "productivity", .reason = ""});
  REQUIRE(bare.notices->count() == 1);
  CHECK(bare.notices->told.front().body.find("Tampoco pude guardarla como recordatorio.") != std::string::npos);
  CHECK(bare.notices->told.front().body.find("No pude activar productivity (no se pudo completar).") == 0);
}

TEST_CASE("a notice that cannot be delivered does not undo the work")
{
  World world("undelivered.db");
  world.notices->delivers = false;
  world.service->offered(world.offerFor(kOwner));
  const int64_t id = world.lastId();
  world.service->accepted({.userId = kOwner, .module = "productivity"});
  world.service->moduleActivated("productivity");
  CHECK(world.stateOf(id) == "done");
}

TEST_CASE("unanswered offers expire, stale waiting intents fail into reminders and a module that came up unnoticed runs its intents")
{
  World world("sweep.db");
  world.service->offered(world.offerFor(kOwner));
  const int64_t unanswered = world.lastId();

  world.now += 3601;
  world.service->sweep();
  CHECK(world.stateOf(unanswered) == "expired");

  world.service->offered(world.offerFor(kResident));
  world.service->accepted({.userId = kResident, .module = "productivity"});
  const int64_t stale = world.lastId();
  world.now += 86401;
  world.service->sweep();
  CHECK(world.stateOf(stale) == "failed");
  REQUIRE(world.rows->created.size() == 1);
  CHECK(world.rows->created.front().userId == kResident);
  REQUIRE(world.notices->count() == 1);
  CHECK(world.notices->told.front().body.find("tardó demasiado") != std::string::npos);

  world.service->offered(world.offerFor(kOwner));
  world.service->accepted({.userId = kOwner, .module = "productivity"});
  const int64_t quiet = world.lastId();
  world.service->sweep();
  CHECK(world.stateOf(quiet) == "waiting");
  world.active.insert("productivity");
  world.service->sweep();
  CHECK(world.stateOf(quiet) == "done");
}

TEST_CASE("settled intents are purged after their retention and open ones are not")
{
  World world("purge.db");
  world.service->offered(world.offerFor(kOwner));
  const int64_t expired = world.lastId();
  world.now += 3601;
  world.service->sweep();
  REQUIRE(world.stateOf(expired) == "expired");
  world.service->offered(world.offerFor(kResident));
  const int64_t open = world.lastId();
  world.now += int64_t{86400} * 31;
  world.service->sweep();
  CHECK(world.stateOf(expired) == "missing");
  CHECK(world.stateOf(open) == "expired");
}

TEST_CASE("an intent survives a restart: a new service over the same database completes it")
{
  World world("restart.db");
  world.service->offered(world.offerFor(kOwner));
  const int64_t id = world.lastId();
  world.service->accepted({.userId = kOwner, .module = "productivity"});
  world.service.reset();

  world.service = world.makeService();
  world.service->moduleActivated("productivity");
  CHECK(world.stateOf(id) == "done");
  CHECK(world.runs.size() == 1);
  CHECK(world.notices->count() == 1);
}

TEST_CASE("the worker completes an intent when told the module is up, and fails it when told the install failed")
{
  World world("worker.db");
  world.service->start();
  world.service->offered(world.offerFor(kOwner));
  const int64_t done = world.lastId();
  world.service->accepted({.userId = kOwner, .module = "productivity"});
  CHECK(waitFor([&world, done] { return world.stateOf(done) == "waiting" || world.stateOf(done) == "done"; }));
  world.service->postActivated("productivity");
  CHECK(waitFor([&world, done] { return world.stateOf(done) == "done"; }));

  world.offline.store(true);
  world.service->offered(world.offerFor(kResident, "surveillance"));
  const int64_t failed = world.lastId();
  world.service->accepted({.userId = kResident, .module = "surveillance"});
  world.service->postFailed({.module = "surveillance", .reason = "network"});
  CHECK(waitFor([&world, failed] { return world.stateOf(failed) == "failed"; }));
  world.service->requestStop();
  CHECK(world.service->drained());
}

TEST_CASE("the executor offers the module when a tool of a module that is off is called, and keeps the call once the owner says yes")
{
  World world("end-to-end.db");
  int created = 0;
  Json::Value stored;
  ToolRegistry registry;
  registry.registerTool(tool_stubs::stub(
      {.name = "calendar.create_event",
       .capability = "agenda.write",
       .handler = [&](const tools::ToolCall& call) {
         ++created;
         stored = call.arguments;
         return tool_stubs::okResult("Agendado: «Reunión con Pedro».");
       },
       .module = "productivity",
       .schema = argus::mcp::schema::object({{.name = "title", .schema = argus::mcp::schema::text(), .required = true}})}));
  int enabled = 0;
  registry.registerTool(tool_stubs::stub(
      {.name = "modules.enable",
       .capability = "modules.manage",
       .handler = [&enabled](const tools::ToolCall&) {
         ++enabled;
         return tool_stubs::okResult("Estoy activando Productividad.");
       },
       .module = "core",
       .schema = argus::mcp::schema::object({{.name = "module", .schema = argus::mcp::schema::text(), .required = true}})}));
  ToolExecutor executor(registry);
  executor.attachLedger(world.service);

  ModuleFlag off{.id = "productivity", .enabled = false};
  off.name = {.es = "Productividad", .en = "Productivity"};
  const ToolAudience audience{.role = UserRole::Owner, .modules = ModuleSnapshot({off})};

  tools::ToolCall ask;
  ask.name = "calendar.create_event";
  ask.arguments["title"] = "Reunión con Pedro";
  ask.context.userId = kOwner;
  ask.context.lang = "es";
  ask.context.sessionId = "voice-7";
  ask.context.utterance = "agéndame una reunión con Pedro";
  ask.context.turn = 1;
  const auto offered = executor.execute(ask, audience);
  CHECK(offered.code == "module_inactive");
  CHECK(created == 0);
  const int64_t id = world.lastId();
  CHECK(world.stateOf(id) == "offered");

  tools::ToolCall yes;
  yes.name = "modules.enable";
  yes.arguments["module"] = "productivity";
  yes.context.userId = kOwner;
  yes.context.lang = "es";
  yes.context.utterance = "sí, actívala";
  yes.context.turn = 2;
  CHECK(executor.execute(yes, audience).ok);
  CHECK(enabled == 1);
  CHECK(world.stateOf(id) == "waiting");

  world.service->moduleActivated("productivity");
  CHECK(world.runs.size() == 1);
  CHECK(world.runs.front().call.name == "calendar.create_event");
  CHECK(world.stateOf(id) == "done");
}

TEST_CASE("the feed turns the enabled set and job failures into signals and ignores everything else")
{
  const auto signals = ModuleIntentFeed::decode(
      R"({"kind":"enabled","version":4,"settled":true,"at":1,"modules":[
           {"id":"core","enabled":true,"lifecycle":"active"},
           {"id":"productivity","enabled":true,"lifecycle":"active"},
           {"id":"surveillance","enabled":false,"lifecycle":"disabled"},
           {"id":"agronomy","enabled":true,"lifecycle":"disabled"}]})");
  REQUIRE(signals.size() == 2);
  CHECK(signals[0].kind == ModuleSignal::Kind::Activated);
  CHECK(signals[0].module == "core");
  CHECK(signals[1].module == "productivity");

  const auto failed = ModuleIntentFeed::decode(
      R"({"kind":"module","version":5,"settled":true,"at":1,"module":{"id":"surveillance","lifecycle":"not_installed",
          "job":{"id":9,"kind":"install","state":"failed","reason":"disk_full"}}})");
  REQUIRE(failed.size() == 1);
  CHECK(failed.front().kind == ModuleSignal::Kind::Failed);
  CHECK(failed.front().module == "surveillance");
  CHECK(failed.front().reason == "disk_full");

  const auto cancelled = ModuleIntentFeed::decode(
      R"({"kind":"module","module":{"id":"surveillance","job":{"state":"cancelled"}}})");
  REQUIRE(cancelled.size() == 1);
  CHECK(cancelled.front().reason == "cancelled");

  CHECK(ModuleIntentFeed::decode(R"({"kind":"module","module":{"id":"surveillance","job":{"kind":"purge","state":"failed"}}})").empty());
  CHECK(ModuleIntentFeed::decode(R"({"kind":"module","module":{"id":"surveillance","job":{"kind":"install","state":"downloading"}}})").empty());
  CHECK(ModuleIntentFeed::decode(R"({"kind":"module","module":{"id":"surveillance","job":null}})").empty());
  CHECK(ModuleIntentFeed::decode(R"({"kind":"enabled","settled":false,"modules":[{"id":"core","enabled":true}]})").empty());
  CHECK(ModuleIntentFeed::decode(R"({"kind":"other"})").empty());
  CHECK(ModuleIntentFeed::decode("not json").empty());
  CHECK(ModuleIntentFeed::decode("[1]").empty());
}

TEST_CASE("the feed follows the settings module stream with its own durable")
{
  const auto config = ModuleIntentFeed::defaults();
  CHECK(config.stream == std::string(nats_subject::kSettingsModuleStream));
  CHECK(config.subject == std::string(nats_subject::kSettingsModule));
  CHECK(config.durable == "argus-llm-intents");
}
