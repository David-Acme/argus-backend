#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <feature/fanout/services/change-feed-consumer.hxx>
#include <nats/live-broker.hxx>
#include <nats/nats-bus.hxx>
#include <sqlite/db-service.hxx>
#include <sync/module-audit-event.hxx>
#include <sync/table-name.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_SYNC_SCHEMA_PATH
#error "ARGUS_SYNC_SCHEMA_PATH must point at the sync schema.sql"
#endif

namespace
{
int streamCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

std::string isolatedName(const std::string& prefix)
{
  return prefix + "-" + std::to_string(::getpid()) + "-" +
         std::to_string(streamCounter());
}

class TempDb
{
public:
  TempDb() : path_(isolatedName("change-feed-live-test") + ".db") {}

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

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}

bool waitForSql(const std::string& sql, const std::string& expected,
                std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (scalar(sql) == expected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return scalar(sql) == expected;
}

Json::Value record(int64_t id, const std::string& name)
{
  Json::Value value;
  value["id"] = static_cast<Json::Int64>(id);
  value["name"] = name;
  return value;
}

Json::Value moduleAudit(int64_t recordId)
{
  ModuleAuditEvent event;
  event.recordId = recordId;
  event.tableName = TableName::Camera;
  event.changes = JsonDiff::createFlatDiff(record(recordId, "before"),
                                           record(recordId, "after"));
  event.createUserId = 42;
  event.eventTimestamp = 1735689600000;
  return event.toJson();
}

struct PublishInput
{
  NatsBus& bus;
  std::string subject;
  int64_t recordId{0};
};

bool publishChange(const PublishInput& input)
{
  return input.bus.publishWithMsgId(
      {.subject = input.subject,
       .payload = json_util::toString(moduleAudit(input.recordId)),
       .msgId = "changefeed-live:" + std::to_string(input.recordId)});
}

constexpr const char* kAuditCount = "SELECT COUNT(*) FROM audit_log";
}

TEST_CASE("a durable change feed drains what arrived while it was detached" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  const char* url = broker.c_str();

  const TempDb db;
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_SYNC_SCHEMA_PATH));

  NatsBus bus;
  NatsBus::Options options;
  options.url = url;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));

  const std::string stream = isolatedName("argus-test-changefeed");
  const std::string subject = stream + ".change";
  const std::string durable = isolatedName("test-changefeed");
  REQUIRE(bus.ensureStream({.name = stream,
                            .subjects = {subject},
                            .maxAgeNs = 3600LL * 1000000000,
                            .duplicatesNs = 120LL * 1000000000}));

  AuditFanOut auditFanOut;
  const std::vector<change_feed::Feed> feeds{
      {.stream = stream,
       .subject = subject,
       .durable = durable,
       .maxAckPending = NatsBus::kOrderedMaxAckPending}};

  std::optional<ChangeFeedConsumer> attached;
  attached.emplace(
      ChangeFeedConsumer::Dependencies{.bus = &bus, .auditFanOut = &auditFanOut},
      ChangeFeedConsumer::Config{.feeds = feeds, .maxDeliver = 5});
  attached->start();

  REQUIRE(publishChange({.bus = bus, .subject = subject, .recordId = 101}));
  REQUIRE(waitForSql(kAuditCount, "1", std::chrono::seconds(20)));
  CHECK(scalar("SELECT record_id FROM audit_log") == "101");

  attached->stop();
  attached.reset();
  std::this_thread::sleep_for(std::chrono::seconds(1));
  REQUIRE(publishChange({.bus = bus, .subject = subject, .recordId = 102}));
  std::this_thread::sleep_for(std::chrono::seconds(2));
  CHECK(scalar(kAuditCount) == "1");

  std::optional<ChangeFeedConsumer> resumed;
  resumed.emplace(
      ChangeFeedConsumer::Dependencies{.bus = &bus, .auditFanOut = &auditFanOut},
      ChangeFeedConsumer::Config{.feeds = feeds, .maxDeliver = 5});
  resumed->start();
  REQUIRE(waitForSql(kAuditCount, "2", std::chrono::seconds(20)));
  CHECK(scalar("SELECT record_id FROM audit_log WHERE record_id = 102") ==
        "102");

  resumed->stop();
  resumed.reset();
  bus.drain();
}
