#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/repositories/memory-graph/memory-graph-repository.hxx>
#include <shared/services/encounter-closed/encounter-closed-consumer.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
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

int nameCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

std::string uniqueStem(const std::string& prefix)
{
  return prefix + "-" + std::to_string(::getpid()) + "-" +
         std::to_string(nameCounter());
}

class TempDir
{
public:
  explicit TempDir(const std::string& stem)
      : path_(std::filesystem::temp_directory_path() / stem)
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

void writeConfig(const std::string& path, const std::string& dbPath)
{
  std::ofstream out(path, std::ios::trunc);
  out << "[database]\nfile = \"" << dbPath << "\"\n"
      << "[memory]\nschema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
      << "create_face_vec = false\n";
}

struct SharedCapture
{
  void record(const EncounterCaptureInput& input)
  {
    {
      std::lock_guard lock(mutex);
      calls.push_back(input);
    }
    changed.notify_all();
  }

  bool waitFor(size_t expected, std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, timeout, [&] {
      return calls.size() >= expected;
    });
  }

  size_t size()
  {
    std::lock_guard lock(mutex);
    return calls.size();
  }

  std::mutex mutex;
  std::condition_variable changed;
  std::vector<EncounterCaptureInput> calls;
};

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

struct PublishInput
{
  NatsBus& bus;
  std::string subject;
  std::string eventId;
  std::string payload;
};

bool publishEncounter(const PublishInput& input)
{
  const std::string body = input.payload.empty()
                               ? encounterPayload(input.eventId)
                               : input.payload;
  return input.bus.publishWithMsgId(
      {.subject = input.subject,
       .payload = body,
       .msgId = "encounter-closed-test:" + input.eventId});
}

struct ConsumerInput
{
  NatsBus* bus{nullptr};
  SqliteGraph* graph{nullptr};
  MemoryGraphRepository* repository{nullptr};
  SharedCapture* shared{nullptr};
  std::string stream;
  std::string durable;
  std::string subject;
  int maxDeliver{10};
  int poisonMaxAttempts{3};
  bool poison{false};
};

EncounterClosedConsumer makeConsumer(const ConsumerInput& input)
{
  EncounterClosedConsumer::Dependencies dependencies;
  dependencies.bus = input.bus;
  dependencies.graph = input.graph;
  dependencies.repository = input.repository;
  if (input.poison) {
    dependencies.capture = [](const EncounterCaptureInput&) -> int64_t {
      throw std::runtime_error("poison capture");
    };
  }
  else {
    SharedCapture* shared = input.shared;
    dependencies.capture = [shared](const EncounterCaptureInput& capture) {
      shared->record(capture);
      return 11;
    };
  }
  return EncounterClosedConsumer(
      std::move(dependencies),
      {.stream = input.stream,
       .durable = input.durable,
       .subject = input.subject,
       .maxDeliver = input.maxDeliver,
       .poisonMaxAttempts = input.poisonMaxAttempts,
       .ownerUserId = 7,
       .lang = "es"});
}
} // namespace

TEST_CASE("encounter fan-out captures exactly once with inbox dedup")
{
  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url == nullptr || *url == '\0') {
    MESSAGE("ARGUS_NATS_URL not set; encounter live check skipped");
    return;
  }

  const TempDir dir(uniqueStem("encounter-closed-live-test"));
  writeConfig(dir.file("config.toml"), dir.file("memory.db"));
  ConfigService::load(dir.file("config.toml"));

  SqliteGraph graph;
  REQUIRE(graph.open(dir.file("memory.db")));
  graph.applySchema();
  MemoryGraphRepository repository;

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  NatsBus bus;
  NatsBus::Options options;
  options.url = url;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));

  const std::string stream = uniqueStem("argus-test-encounter");
  const std::string subject = stream + ".events";
  const std::string durable = uniqueStem("test-encounter");
  REQUIRE(bus.ensureStream({.name = stream,
                            .subjects = {subject},
                            .maxAgeNs = 3600LL * 1000000000,
                            .duplicatesNs = 120LL * 1000000000}));

  SharedCapture first;
  auto consumer = makeConsumer({.bus = &bus,
                                .graph = &graph,
                                .repository = &repository,
                                .shared = &first,
                                .stream = stream,
                                .durable = durable,
                                .subject = subject,
                                .maxDeliver = 10,
                                .poisonMaxAttempts = 3,
                                .poison = false});

  REQUIRE(publishEncounter(
      {.bus = bus, .subject = subject, .eventId = "live:1", .payload = {}}));
  REQUIRE(publishEncounter(
      {.bus = bus, .subject = subject, .eventId = "live:2", .payload = {}}));
  REQUIRE(publishEncounter(
      {.bus = bus, .subject = subject, .eventId = "live:1", .payload = {}}));
  consumer.start();
  REQUIRE(first.waitFor(2, std::chrono::seconds(20)));
  CHECK(first.size() == 2);

  consumer.stop();
  REQUIRE(publishEncounter(
      {.bus = bus, .subject = subject, .eventId = "live:3", .payload = {}}));
  std::this_thread::sleep_for(std::chrono::seconds(2));
  CHECK(first.size() == 2);
  auto resumed = makeConsumer({.bus = &bus,
                               .graph = &graph,
                               .repository = &repository,
                               .shared = &first,
                               .stream = stream,
                               .durable = durable,
                               .subject = subject,
                               .maxDeliver = 10,
                               .poisonMaxAttempts = 3,
                               .poison = false});
  resumed.start();
  REQUIRE(first.waitFor(3, std::chrono::seconds(20)));
  CHECK(first.size() == 3);
  REQUIRE(drogon::sync_wait(resumed.handlePayload(
      encounterPayload("live:1"))) == EncounterDisposition::Ack);
  CHECK(first.size() == 3);

  REQUIRE(publishEncounter({.bus = bus,
                            .subject = subject,
                            .eventId = "live:bad",
                            .payload = "not-json"}));
  REQUIRE(publishEncounter({.bus = bus,
                            .subject = subject,
                            .eventId = "live:4",
                            .payload = encounterPayload("live:4")}));
  REQUIRE(first.waitFor(4, std::chrono::seconds(20)));
  CHECK(first.size() == 4);

  resumed.stop();
  auto poison = makeConsumer({.bus = &bus,
                              .graph = &graph,
                              .repository = &repository,
                              .shared = nullptr,
                              .stream = stream,
                              .durable = uniqueStem("test-encounter"),
                              .subject = subject,
                              .maxDeliver = 2,
                              .poisonMaxAttempts = 100,
                              .poison = true});
  poison.start();
  REQUIRE(publishEncounter(
      {.bus = bus, .subject = subject, .eventId = "live:5", .payload = {}}));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  int64_t attempts = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    SqliteStmt stmt;
    if (stmt.prepare(graph.handle(),
                     "SELECT attempts FROM encounter_closed_inbox WHERE "
                     "event_id = 'live:5'") &&
        stmt.step() == SQLITE_ROW)
      attempts = stmt.columnInt64(0);
    if (attempts >= 2)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CHECK(attempts == 2);
  std::this_thread::sleep_for(std::chrono::seconds(3));
  {
    SqliteStmt stmt;
    int64_t settled = -1;
    if (stmt.prepare(graph.handle(),
                     "SELECT attempts FROM encounter_closed_inbox WHERE "
                     "event_id = 'live:5'") &&
        stmt.step() == SQLITE_ROW)
      settled = stmt.columnInt64(0);
    CHECK(settled == 2);
  }
  poison.stop();

  graph.close();
  bus.drain();
  drogon::app().quit();
  runner.join();
}
