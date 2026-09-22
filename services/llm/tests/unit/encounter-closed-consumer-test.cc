#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/repositories/memory-graph/memory-graph-repository.hxx>
#include <shared/services/encounter-closed/encounter-closed-consumer.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <config/config-service.hxx>
#include <text/json-util.hxx>
#include <text/sha256.hxx>
#include <sqlite/sqlite-stmt.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
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

// Runs the app and stops it however the case body leaves. A joinable
// std::thread destroyed by unwinding calls std::terminate, which reports an
// ordinary statement failure as a SIGABRT with no assertion behind it.
class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    // Drogon reports the app running before its main loop is looping, and a
    // loop that has not begun cannot be stopped: trantor's loop() clears the
    // quit flag again as it starts. Waiting for it to loop is what makes the
    // quit below take effect — detaching in that window left the app's thread
    // running past the end of the process, measured as SIGSEGV inside
    // EventLoop::loop() in 3 of 20 runs of a forced constructor throw.
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    // A boot that never reached the loop at all is left to the process: it
    // cannot be asked to stop, and joining it would block for ever.
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

void writeConfig(const std::string& path, const std::string& dbPath)
{
  std::ofstream out(path, std::ios::trunc);
  out << "[database]\nfile = \"" << dbPath << "\"\n"
      << "[memory]\nschema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
      << "create_face_vec = false\n";
}

std::string encounterPayload(const std::string& eventId,
                             const std::string& grade = "medium")
{
  Json::Value json(Json::objectValue);
  json["eventId"] = eventId;
  json["encounterId"] = Json::Int64(9);
  json["cameraId"] = Json::Int64(3);
  json["personId"] = Json::Int64(0);
  json["grade"] = grade;
  json["durationS"] = Json::Int64(45);
  json["closedAt"] = Json::Int64(1700000000);
  return json_util::toString(json);
}

struct RecordedCapture
{
  std::vector<EncounterCaptureInput> calls;
  bool fail{false};
  bool reject{false};
};

EncounterClosedConsumer::Config consumerConfig()
{
  return {.stream = "test-stream",
          .durable = "test-durable",
          .subject = "test-subject",
          .maxDeliver = 10,
          .poisonMaxAttempts = 3,
          .ownerUserId = 7,
          .lang = "es"};
}
} // namespace

TEST_CASE("the encounter consumer captures exactly once per receipt")
{
  const TempDir dir("encounter-consumer-test");
  writeConfig(dir.file("config.toml"), dir.file("memory.db"));
  ConfigService::load(dir.file("config.toml"));

  SqliteGraph graph;
  REQUIRE(graph.open(dir.file("memory.db")));
  graph.applySchema();
  MemoryGraphRepository repository;

  RecordedCapture captured;
  EncounterClosedConsumer consumer(
      {.bus = nullptr,
       .graph = &graph,
       .repository = &repository,
       .capture =
           [&captured](const EncounterCaptureInput& input) {
             captured.calls.push_back(input);
             if (captured.fail)
               throw std::runtime_error("poison capture");
             return captured.reject ? 0 : 11;
           }},
      consumerConfig());

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  CHECK(drogon::sync_wait(
      consumer.handlePayload(encounterPayload("closed:1"))) ==
        EncounterDisposition::Ack);
  REQUIRE(captured.calls.size() == 1);
  CHECK(captured.calls[0].cameraId == 3);
  CHECK(captured.calls[0].ownerUserId == 7);
  CHECK(captured.calls[0].lang == "es");
  CHECK(drogon::sync_wait(
      consumer.handlePayload(encounterPayload("closed:1"))) ==
        EncounterDisposition::Ack);
  CHECK(captured.calls.size() == 1);
  CHECK(scalarText(graph.handle(), "SELECT status FROM encounter_closed_inbox "
                                   "WHERE event_id = 'closed:1'") ==
        "dispatched");

  const std::string crashPayload = encounterPayload("closed:2");
  const std::string crashFingerprint = argus::hash::sha256Hex(
      json_util::toString(json_util::fromString(crashPayload)));
  CHECK(repository.claimEncounterClosed(graph.handle(),
                                        {.eventId = "closed:2",
                                         .fingerprint = crashFingerprint,
                                         .at = 100})
            .duplicate == false);
  CHECK(drogon::sync_wait(
      consumer.handlePayload(crashPayload)) ==
        EncounterDisposition::Ack);
  CHECK(captured.calls.size() == 2);

  auto tampered = encounterPayload("closed:3");
  CHECK(drogon::sync_wait(consumer.handlePayload(tampered)) ==
        EncounterDisposition::Ack);
  Json::Value altered = json_util::fromString(tampered);
  altered["grade"] = "critical";
  CHECK(drogon::sync_wait(
      consumer.handlePayload(json_util::toString(altered))) ==
        EncounterDisposition::Ack);
  CHECK(captured.calls.size() == 3);
  CHECK(scalarText(graph.handle(), "SELECT status FROM encounter_closed_inbox "
                                   "WHERE event_id = 'closed:3'") ==
        "conflict");

  captured.fail = true;
  CHECK(drogon::sync_wait(
      consumer.handlePayload(encounterPayload("closed:4"))) ==
        EncounterDisposition::Nak);
  CHECK(drogon::sync_wait(
      consumer.handlePayload(encounterPayload("closed:4"))) ==
        EncounterDisposition::Nak);
  CHECK(drogon::sync_wait(
      consumer.handlePayload(encounterPayload("closed:4"))) ==
        EncounterDisposition::Term);
  CHECK(scalarText(graph.handle(), "SELECT status FROM encounter_closed_inbox "
                                   "WHERE event_id = 'closed:4'") ==
        "dead_lettered");
  captured.fail = false;

  captured.reject = true;
  CHECK(drogon::sync_wait(
      consumer.handlePayload(encounterPayload("closed:5"))) ==
        EncounterDisposition::Nak);
  captured.reject = false;

  CHECK(drogon::sync_wait(consumer.handlePayload("not-json")) ==
        EncounterDisposition::Term);
  CHECK(drogon::sync_wait(consumer.handlePayload("{\"cameraId\":3}")) ==
        EncounterDisposition::Term);

  EncounterClosedConsumer unconfigured(
      {.bus = nullptr,
       .graph = nullptr,
       .repository = nullptr,
       .capture = {}},
      consumerConfig());
  CHECK(drogon::sync_wait(
      unconfigured.handlePayload(encounterPayload("closed:6"))) ==
        EncounterDisposition::Nak);

  graph.close();
}
