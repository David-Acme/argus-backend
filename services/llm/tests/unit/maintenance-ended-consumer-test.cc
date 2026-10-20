#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/encounter-closed/services/maintenance-ended-consumer.hxx>
#include <feature/memory/repositories/memory-graph/memory-graph-repository.hxx>
#include <feature/memory/services/memory/sqlite-graph.hxx>
#include <config/config-service.hxx>
#include <text/json-util.hxx>
#include <sqlite/sqlite-stmt.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#error "ARGUS_TEST_MEMORY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
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

int nameCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

class TempDir
{
public:
  explicit TempDir(const std::string& stem)
      : path_(std::filesystem::temp_directory_path() /
              (stem + "-" + std::to_string(::getpid()) + "-" +
               std::to_string(nameCounter())))
  {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ~TempDir() { std::filesystem::remove_all(path_); }

  std::string file(const std::string& name) const
  {
    return (path_ / name).string();
  }

private:
  std::filesystem::path path_;
};

std::string scalarText(sqlite3* db, const std::string& sql)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, sql.c_str()))
    return {};
  if (stmt.step() != SQLITE_ROW)
    return {};
  return stmt.columnText(0);
}

struct ConfigFiles
{
  std::string path;
  std::string dbPath;
};

void writeConfig(const ConfigFiles& files)
{
  const std::string& path = files.path;
  const std::string& dbPath = files.dbPath;
  std::ofstream out(path, std::ios::trunc);
  out << "[database]\nfile = \"" << dbPath << "\"\n"
      << "[memory]\nschema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
      << "create_face_vec = false\n";
}

constexpr int64_t kStartedAt = 1792422300;
constexpr int64_t kEndedAt = kStartedAt + 35 * 60;
constexpr int kLima = -5 * 3600;

Json::Value windowJson(const std::string& eventId)
{
  Json::Value json(Json::objectValue);
  json["eventId"] = eventId;
  json["maintenanceId"] = Json::Int64(12);
  json["environmentId"] = Json::Int64(1);
  json["environment"] = "";
  Json::Value ids(Json::arrayValue);
  ids.append(Json::Int64(3));
  ids.append(Json::Int64(4));
  json["cameraIds"] = ids;
  Json::Value names(Json::arrayValue);
  names.append("Patio");
  names.append("Garaje");
  json["cameras"] = names;
  json["declaredBy"] = Json::Int64(2);
  json["declaredName"] = "Luis";
  json["declaredRole"] = "resident";
  json["source"] = "assistant";
  json["startedAt"] = Json::Int64(kStartedAt);
  json["endedAt"] = Json::Int64(kEndedAt);
  json["endReason"] = "done";
  return json;
}

std::string windowPayload(const std::string& eventId)
{
  return json_util::toString(windowJson(eventId));
}

MaintenanceEndedEvent eventOf(const Json::Value& json)
{
  auto event = MaintenanceEndedEvent::fromJson(json);
  REQUIRE(event.has_value());
  return event.value_or(MaintenanceEndedEvent{});
}

MaintenanceEndedConsumer::Config consumerConfig()
{
  return {.stream = "test-stream",
          .durable = "test-durable",
          .subject = "test-subject",
          .maxDeliver = 10,
          .poisonMaxAttempts = 3,
          .ownerUserId = 7,
          .lang = "es"};
}

struct Captured
{
  std::vector<std::string> summaries;
  std::vector<int64_t> owners;
  bool fail{false};
};
}

TEST_CASE("a window reads back who, when and which cameras, and a broken one is refused")
{
  const MaintenanceEndedEvent event = eventOf(windowJson("guard-maintenance-ended:12"));
  CHECK(event.maintenanceId == 12);
  CHECK(event.declaredName == "Luis");
  CHECK(event.cameras == std::vector<std::string>{"Patio", "Garaje"});
  CHECK(event.startedAt == kStartedAt);
  CHECK(event.endedAt == kEndedAt);

  Json::Value backwards = windowJson("guard-maintenance-ended:13");
  backwards["endedAt"] = Json::Int64(kStartedAt - 1);
  CHECK_FALSE(MaintenanceEndedEvent::fromJson(backwards).has_value());
  Json::Value nameless = windowJson("");
  CHECK_FALSE(MaintenanceEndedEvent::fromJson(nameless).has_value());
  CHECK_FALSE(MaintenanceEndedEvent::fromJson(Json::Value("x")).has_value());
}

TEST_CASE("the episode says who declared it, on which cameras and when, in the owner's language and clock")
{
  const MaintenanceEndedEvent event = eventOf(windowJson("guard-maintenance-ended:12"));
  CHECK(maintenance_episode::summary({.event = event, .lang = "es", .utcOffsetSeconds = kLima}) ==
        "Mantenimiento de cámaras (visita del técnico o limpieza) declarado por Luis, en las cámaras Patio y Garaje, "
        "el lunes 19 de octubre de 10:05 a 10:40 (35 min).");
  CHECK(maintenance_episode::summary({.event = event, .lang = "en", .utcOffsetSeconds = kLima}) ==
        "Camera maintenance (technician visit or cleaning) declared by Luis, on the Patio and Garaje cameras, "
        "on Monday, October 19 from 10:05 to 10:40 (35 min).");

  Json::Value expired = windowJson("guard-maintenance-ended:14");
  expired["endReason"] = "expired";
  expired["declaredName"] = "";
  expired["environment"] = "Casa de campo";
  Json::Value one(Json::arrayValue);
  one.append("Patio");
  expired["cameras"] = one;
  CHECK(maintenance_episode::summary({.event = eventOf(expired), .lang = "es", .utcOffsetSeconds = kLima}) ==
        "Mantenimiento de cámaras (visita del técnico o limpieza) declarado por alguien de casa, en la cámara Patio "
        "de Casa de campo, el lunes 19 de octubre de 10:05 a 10:40 (35 min). Terminó solo al acabarse el tiempo.");
}

TEST_CASE("each window is captured once in the owner's memory, and when the technician came can be found again")
{
  const TempDir dir("maintenance-consumer-test");
  writeConfig({.path = dir.file("config.toml"), .dbPath = dir.file("memory.db")});
  ConfigService::load(dir.file("config.toml"));

  SqliteGraph graph;
  REQUIRE(graph.open(dir.file("memory.db")));
  graph.applySchema();
  MemoryGraphRepository repository;

  Captured captured;
  MaintenanceEndedConsumer consumer(
      {.bus = nullptr,
       .graph = &graph,
       .repository = &repository,
       .capture =
           [&captured, &graph, &repository](const MaintenanceCaptureInput& input) -> int64_t {
             if (captured.fail)
               throw std::runtime_error("poison capture");
             const std::string summary =
                 maintenance_episode::summary({.event = input.event, .lang = input.lang, .utcOffsetSeconds = kLima});
             captured.summaries.push_back(summary);
             captured.owners.push_back(input.ownerUserId);
             return repository.recordEpisode(graph.handle(), {.kind = "maintenance",
                                                              .summary = summary,
                                                              .actor = "argus-guard",
                                                              .occurredAt = input.event.endedAt,
                                                              .sessionId = "",
                                                              .lang = input.lang,
                                                              .scope = "user",
                                                              .refId = input.ownerUserId,
                                                              .salience = 0.5F,
                                                              .sourceId = std::nullopt,
                                                              .mentionEntityIds = {}});
           }},
      consumerConfig());

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  CHECK(drogon::sync_wait(consumer.handlePayload(windowPayload("guard-maintenance-ended:12"))) ==
        MaintenanceDisposition::Ack);
  CHECK(drogon::sync_wait(consumer.handlePayload(windowPayload("guard-maintenance-ended:12"))) ==
        MaintenanceDisposition::Ack);
  REQUIRE(captured.summaries.size() == 1);
  CHECK(captured.owners.front() == 7);
  CHECK(scalarText(graph.handle(), "SELECT status FROM encounter_closed_inbox "
                                   "WHERE event_id = 'guard-maintenance-ended:12'") == "dispatched");

  const auto found = repository.ftsEpisodes(
      graph.handle(), {.match = "\"cuando\" OR \"vino\" OR \"el\" OR \"tecnico\"", .scope = "user", .refId = 7, .limit = 2});
  REQUIRE(found.size() == 1);
  CHECK(found.front().summary.find("10:05") != std::string::npos);
  CHECK(repository
            .ftsEpisodes(graph.handle(), {.match = "\"tecnico\"", .scope = "user", .refId = 8, .limit = 2})
            .empty());

  captured.fail = true;
  for (int attempt = 0; attempt < 2; ++attempt)
    CHECK(drogon::sync_wait(consumer.handlePayload(windowPayload("guard-maintenance-ended:15"))) ==
          MaintenanceDisposition::Nak);
  CHECK(drogon::sync_wait(consumer.handlePayload(windowPayload("guard-maintenance-ended:15"))) ==
        MaintenanceDisposition::Term);
  captured.fail = false;

  CHECK(drogon::sync_wait(consumer.handlePayload("not-json")) == MaintenanceDisposition::Term);

  MaintenanceEndedConsumer unconfigured({.bus = nullptr, .graph = nullptr, .repository = nullptr, .capture = {}},
                                        consumerConfig());
  CHECK(drogon::sync_wait(unconfigured.handlePayload(windowPayload("guard-maintenance-ended:16"))) ==
        MaintenanceDisposition::Nak);

  graph.close();
}
