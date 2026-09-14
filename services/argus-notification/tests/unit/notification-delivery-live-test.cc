#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <notification/nats-notification-delivery-sink.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#ifndef ARGUS_NOTIFICATION_SCHEMA
#error "ARGUS_NOTIFICATION_SCHEMA must point at database/schema.sql"
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

// Unique database per execution, removed with its WAL/SHM on destruction.
class TempDb
{
public:
  TempDb()
      : path_("delivery-fanout-" + std::to_string(::getpid()) + "-" +
              std::to_string(nameCounter()) + ".db")
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

int64_t scalarCount(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return -1;
  return rows.front()["total"].as<int64_t>();
}

int64_t pendingCount()
{
  return scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                     "WHERE status = 'pending'");
}

int64_t sentCount()
{
  return scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                     "WHERE status = 'sent'");
}

int64_t rowCount()
{
  return scalarCount("SELECT COUNT(*) AS total FROM notification");
}

NotificationBatchInput batchWith(const std::string& commandId)
{
  NotificationBatchInput batch;
  batch.userIds = {11, 12};
  batch.notification.type = "camera";
  batch.notification.title = "Alley";
  batch.notification.body = "Person detected";
  batch.commandId = commandId;
  return batch;
}

std::shared_ptr<NatsBus> connectBus(const std::string& url)
{
  auto bus = std::make_shared<NatsBus>();
  NatsBus::Options options;
  options.url = url;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  if (!bus->connect(options))
    return nullptr;
  return bus;
}

NatsNotificationDeliverySink::Config sinkConfig(const std::string& stream,
                                                const std::string& subject)
{
  return {.stream = stream, .subject = subject};
}
} // namespace

TEST_CASE("create settles fan-out only on broker ack, across an outage")
{
  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url == nullptr || *url == '\0') {
    MESSAGE("ARGUS_NATS_URL not set; delivery fan-out live check skipped");
    return;
  }

  const TempDb db;
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_NOTIFICATION_SCHEMA));

  const std::string stream = "argus-test-fanout-" +
                             std::to_string(::getpid()) + "-" +
                             std::to_string(nameCounter());
  const std::string subject = stream + ".events";

  auto deadBus = std::make_shared<NatsBus>();
  NatsBus::Options deadOptions;
  deadOptions.url = "nats://127.0.0.1:59999";
  deadOptions.reconnectWaitMs = 50;
  deadOptions.maxReconnects = 1;
  CHECK_FALSE(deadBus->connect(deadOptions));
  const auto deadSink = std::make_shared<NatsNotificationDeliverySink>(
      deadBus, sinkConfig(stream, subject));
  const NotificationService offline({.deliverySink = deadSink,
                                     .pushSink = {},
                                     .pushRequired = false});
  const auto offlineOutcome = drogon::sync_wait(
      offline.createManyAndEmit(batchWith("live-fanout-0")));
  CHECK_FALSE(offlineOutcome.duplicate);
  CHECK(offlineOutcome.createdCount == 2);
  CHECK(rowCount() == 2);
  CHECK(pendingCount() == 2);
  deadBus->drain();

  auto bus = connectBus(url);
  REQUIRE(bus != nullptr);
  auto sink = std::make_shared<NatsNotificationDeliverySink>(
      bus, sinkConfig(stream, subject));
  REQUIRE(sink->ensureStream());
  const NotificationService service({.deliverySink = sink,
                                     .pushSink = {},
                                     .pushRequired = false});

  const auto first = drogon::sync_wait(
      service.createManyAndEmit(batchWith("live-fanout-1")));
  CHECK_FALSE(first.duplicate);
  CHECK(first.createdCount == 2);
  CHECK(rowCount() == 4);
  CHECK(sentCount() == 4);

  bus->drain();
  const auto during = drogon::sync_wait(
      service.createManyAndEmit(batchWith("live-fanout-2")));
  CHECK_FALSE(during.duplicate);
  CHECK(rowCount() == 6);
  CHECK(pendingCount() == 2);

  bus = connectBus(url);
  REQUIRE(bus != nullptr);
  sink = std::make_shared<NatsNotificationDeliverySink>(
      bus, sinkConfig(stream, subject));
  REQUIRE(sink->ensureStream());
  const NotificationService reconnected({.deliverySink = sink,
                                         .pushSink = {},
                                         .pushRequired = false});
  drogon::sync_wait(reconnected.deliverPending());
  CHECK(pendingCount() == 0);
  CHECK(sentCount() == 6);
  CHECK(rowCount() == 6);

  bus->drain();
  drogon::app().quit();
  runner.join();
}
