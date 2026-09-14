#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <guard-schema.hxx>
#include <guard-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
int tempCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

class TempDb
{
public:
  explicit TempDb(const char* stem)
      : path_(std::string(stem) + "-" + std::to_string(::getpid()) + "-" +
              std::to_string(tempCounter()) + ".db")
  {
  }

  ~TempDb()
  {
    std::remove(path_.c_str());
    std::remove((path_ + "-wal").c_str());
    std::remove((path_ + "-shm").c_str());
  }

  const std::string& path() const { return path_; }

private:
  std::string path_;
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

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}
} // namespace

// Opt-in live check against a real NATS + JetStream (ARGUS_NATS_URL). It drives
// the real GuardService: a poison observation exhausts deliveries until the
// guard parks it and marks the inbox dead-lettered.
TEST_CASE("the guard service parks a poison observation before the last delivery")
{
  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url == nullptr || *url == '\0') {
    MESSAGE("ARGUS_NATS_URL not set; guard DLQ service check skipped");
    return;
  }

  const TempDb db("guard-dlq-service-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH));
  DbService::client()->execSqlSync("DROP TABLE guard_incident");

  NatsBus bus;
  NatsBus::Options options;
  options.url = url;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));
  static const std::vector<std::string> kSubjects = {"argus.test.guard.dlq"};
  REQUIRE(bus.ensureStream({.name = "ARGUS_GUARD_DLQTEST",
                            .subjects = kSubjects,
                            .maxAgeNs = 3600LL * 1000000000,
                            .duplicatesNs = 0}));

  GuardService::Config config;
  config.enabled = true;
  config.consumerDurable = "dlq-svc-" + std::to_string(::getpid());
  config.maxObservationAttempts = 3;
  config.eventStream = "ARGUS_GUARD_DLQTEST";
  config.eventSubject = "argus.test.guard.dlq";
  config.heartbeatS = 3600;
  config.defaultMode = GuardMode::Home;
  GuardService service({.bus = &bus,
                        .identity = nullptr,
                        .notifications = nullptr,
                        .actions = nullptr,
                        .assessment = nullptr},
                       config);
  service.start();
  std::this_thread::sleep_for(std::chrono::seconds(1));

  Json::Value event(Json::objectValue);
  event["schemaVersion"] = 2;
  event["eventId"] = "poison-service:1";
  event["cameraId"] = 1;
  event["rule"] = "person_day";
  event["severity"] = "info";
  event["trackId"] = Json::Int64(1);
  Json::Value object(Json::objectValue);
  object["class"] = "person";
  object["identity"] = "unknown";
  object["trackId"] = Json::Int64(1);
  Json::Value bbox(Json::objectValue);
  bbox["w"] = 32.0;
  bbox["h"] = 32.0;
  object["bbox"] = bbox;
  Json::Value objects(Json::arrayValue);
  objects.append(object);
  event["objects"] = objects;
  REQUIRE(bus.publish("argus.test.guard.dlq", json_util::toString(event)));

  bool parked = false;
  for (int attempt = 0; attempt < 150 && !parked; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    parked = scalar("SELECT COUNT(*) FROM guard_dead_letter "
                    "WHERE event_id = 'poison-service:1'") == "1";
  }
  CHECK(parked);
  CHECK(scalar("SELECT status FROM guard_observation_inbox "
               "WHERE event_id = 'poison-service:1'") == "dead_lettered");
  const int attempts =
      std::stoi(scalar("SELECT attempts FROM guard_dead_letter "
                       "WHERE event_id = 'poison-service:1'"));
  CHECK(attempts == 2);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  CHECK(scalar("SELECT attempts FROM guard_dead_letter "
               "WHERE event_id = 'poison-service:1'") == "2");

  bus.drain();
  drogon::app().quit();
  runner.join();
}
