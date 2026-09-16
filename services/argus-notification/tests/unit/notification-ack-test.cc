#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/contracts/notification-delivery-sink.hxx>
#include <shared/contracts/user-change-sink.hxx>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <shared/services/sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
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

struct SharedBoot
{
  TempDb db{"notification-ack-test"};
  std::thread runner;

  SharedBoot()
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
    runner = std::thread([] { drogon::app().run(); });
    if (!waitForBoot(std::chrono::seconds(30)))
      throw std::runtime_error("drogon loop did not boot");
    auto client = DbService::client();
    client->execSqlSync(
        "CREATE TABLE notification ("
        "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
        "user_id INTEGER NOT NULL, "
        "type TEXT NOT NULL DEFAULT 'system', "
        "title TEXT NOT NULL DEFAULT '', body TEXT NOT NULL DEFAULT '', "
        "data TEXT NOT NULL DEFAULT '{}', "
        "is_read INTEGER NOT NULL DEFAULT 0, "
        "read_at INTEGER, "
        "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
    client->execSqlSync(
        "CREATE TABLE notification_command ("
        "command_id TEXT NOT NULL PRIMARY KEY, "
        "expected_count INTEGER NOT NULL DEFAULT 0, "
        "fingerprint TEXT NOT NULL DEFAULT '', "
        "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
    client->execSqlSync(
        "CREATE TABLE notification_delivery ("
        "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
        "notification_id INTEGER NOT NULL, "
        "user_id INTEGER NOT NULL DEFAULT 0, "
        "status TEXT NOT NULL DEFAULT 'pending', "
        "attempts INTEGER NOT NULL DEFAULT 0, "
        "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
        "sent_at INTEGER NOT NULL DEFAULT 0, "
        "acked_at INTEGER NOT NULL DEFAULT 0, "
        "created_ms INTEGER NOT NULL DEFAULT 0, "
        "sent_ms INTEGER NOT NULL DEFAULT 0, "
        "acked_ms INTEGER NOT NULL DEFAULT 0, "
        "UNIQUE (notification_id))");
    client->execSqlSync(
        "CREATE TABLE notification_selftest ("
        "id INTEGER NOT NULL PRIMARY KEY CHECK (id = 1), "
        "last_at INTEGER NOT NULL DEFAULT 0, "
        "last_ok INTEGER NOT NULL DEFAULT 0, "
        "last_ms INTEGER NOT NULL DEFAULT 0)");
  }

  ~SharedBoot()
  {
    drogon::app().quit();
    if (runner.joinable())
      runner.join();
  }
};

SharedBoot& sharedBoot()
{
  static SharedBoot boot;
  return boot;
}

int64_t scalarCount(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return -1;
  return rows.front()["total"].as<int64_t>();
}

class RecordingDeliverySink final : public NotificationDeliverySink
{
public:
  bool ensureStream() const override { return true; }

  bool publish(const NotificationDeliveryEvent& event) const override
  {
    published.push_back(event);
    return armed;
  }

  mutable std::vector<NotificationDeliveryEvent> published;
  mutable bool armed{false};
};
} // namespace

TEST_CASE("display confirmations settle against the delivery row")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;

  auto deliverySink = std::make_shared<RecordingDeliverySink>();
  deliverySink->armed = true;
  const NotificationService service(
      {.deliverySink = deliverySink, .pushSink = {}, .pushRequired = false});

  NotificationBatchInput batch;
  batch.userIds = {11, 12};
  batch.notification.type = "camera";
  batch.notification.title = "Gate";
  batch.notification.body = "Motion";
  batch.commandId = "ack-flow";
  const auto outcome = drogon::sync_wait(service.createManyAndEmit(batch));
  REQUIRE(outcome.createdCount == 2);

  const auto ids = DbService::client()->execSqlSync(
      "SELECT id FROM notification WHERE user_id IN (11, 12) ORDER BY id ASC");
  REQUIRE(ids.size() == 2);
  const int64_t firstId = ids.front()["id"].as<int64_t>();
  const int64_t secondId = ids.back()["id"].as<int64_t>();

  CHECK(drogon::sync_wait(service.ackDeliveries(11, {firstId})) == 1);
  CHECK(drogon::sync_wait(service.ackDeliveries(11, {firstId})) == 0);
  CHECK(drogon::sync_wait(service.ackDeliveries(12, {firstId})) == 0);
  CHECK(drogon::sync_wait(service.ackDeliveries(12, {secondId})) == 1);
  CHECK(drogon::sync_wait(service.ackDeliveries(11, {})) == 0);

  const auto ackedRows = DbService::client()->execSqlSync(
      "SELECT COUNT(*) AS total FROM notification_delivery WHERE user_id IN "
      "(11, 12) AND status = 'sent' AND acked_at > 0");
  CHECK(ackedRows.front()["total"].as<int64_t>() == 2);

  const int64_t probesBefore = scalarCount(
      "SELECT COUNT(*) AS total FROM notification WHERE type = 'probe'");
  const SelfTestState probe = drogon::sync_wait(service.runSelfTest());
  CHECK(probe.ok);
  CHECK(probe.at > 0);
  CHECK(probe.ms >= 0);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification WHERE type = "
                    "'probe'") == probesBefore + 1);

  const NotificationRepository repository;
  const SelfTestState stored = drogon::sync_wait(repository.probeState());
  CHECK(stored.at == probe.at);
  CHECK(stored.ok);

  const DeliverySummary summary =
      drogon::sync_wait(service.deliverySummary(0, 86400));
  CHECK(summary.probeAt == probe.at);
  CHECK(summary.probeOk);
  CHECK(summary.probeMs == probe.ms);
  CHECK(summary.latencyMsP50 >= 0);
  CHECK(summary.latencyMsP50 <= summary.latencyMsMax);
}

TEST_CASE("a probe without a broker records its failure visibly")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;

  auto deliverySink = std::make_shared<RecordingDeliverySink>();
  deliverySink->armed = false;
  const NotificationService service(
      {.deliverySink = deliverySink, .pushSink = {}, .pushRequired = false});

  const SelfTestState probe = drogon::sync_wait(service.runSelfTest());
  CHECK_FALSE(probe.ok);
  CHECK(probe.at > 0);

  const NotificationRepository repository;
  const SelfTestState stored = drogon::sync_wait(repository.probeState());
  CHECK(stored.at == probe.at);
  CHECK_FALSE(stored.ok);
}

TEST_CASE("latency percentiles respect the caller window")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;

  auto deliverySink = std::make_shared<RecordingDeliverySink>();
  deliverySink->armed = true;
  const NotificationService service(
      {.deliverySink = deliverySink, .pushSink = {}, .pushRequired = false});

  NotificationBatchInput batch;
  batch.userIds = {21, 22};
  batch.notification.type = "camera";
  batch.notification.title = "Alley";
  batch.notification.body = "Motion";
  batch.commandId = "latency-window";
  REQUIRE(drogon::sync_wait(service.createManyAndEmit(batch)).createdCount ==
          2);

  const auto deliveries = DbService::client()->execSqlSync(
      "SELECT id FROM notification_delivery WHERE user_id IN (21, 22) ORDER "
      "BY id ASC");
  REQUIRE(deliveries.size() == 2);
  const int64_t oldId = deliveries.front()["id"].as<int64_t>();
  const int64_t freshId = deliveries.back()["id"].as<int64_t>();
  DbService::client()->execSqlSync(
      "UPDATE notification_delivery SET sent_ms = created_ms");
  DbService::client()->execSqlSync(
      "UPDATE notification_delivery SET created_ms = 5000000000, sent_ms = "
      "5000000500 WHERE id = " +
      std::to_string(oldId));
  DbService::client()->execSqlSync(
      "UPDATE notification_delivery SET sent_ms = created_ms + 42 WHERE id = " +
      std::to_string(freshId));

  const int64_t nowS = static_cast<int64_t>(std::time(nullptr));
  const DeliverySummary summary =
      drogon::sync_wait(service.deliverySummary(nowS - 3600, 86400));
  CHECK(summary.latencyMsP50 == 42);
  CHECK(summary.latencyMsP95 == 42);
  CHECK(summary.latencyMsMax == 42);
}

TEST_CASE("a refusing broker reports publish-refused with intents pending")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;

  auto deliverySink = std::make_shared<RecordingDeliverySink>();
  deliverySink->armed = false;
  const NotificationService service(
      {.deliverySink = deliverySink, .pushSink = {}, .pushRequired = false});

  NotificationBatchInput batch;
  batch.userIds = {41};
  batch.notification.type = "camera";
  batch.notification.title = "Shed";
  batch.notification.body = "Motion";
  batch.commandId = "refused-flow";
  REQUIRE(drogon::sync_wait(service.createManyAndEmit(batch)).createdCount ==
          1);
  CHECK(drogon::sync_wait(service.deliverPending()) ==
        DeliverPendingOutcome::PublishRefused);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                    "WHERE user_id = 41 AND status = 'pending'") == 1);

  deliverySink->armed = true;
  CHECK(drogon::sync_wait(service.deliverPending()) ==
        DeliverPendingOutcome::Settled);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                    "WHERE user_id = 41 AND status = 'sent'") == 1);
}

TEST_CASE("synthetic probes never pollute delivery metrics")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;

  auto deliverySink = std::make_shared<RecordingDeliverySink>();
  deliverySink->armed = true;
  const NotificationService service(
      {.deliverySink = deliverySink, .pushSink = {}, .pushRequired = false});

  NotificationBatchInput batch;
  batch.userIds = {31};
  batch.notification.type = "camera";
  batch.notification.title = "Porch";
  batch.notification.body = "Motion";
  batch.commandId = "probe-exclusion";
  REQUIRE(drogon::sync_wait(service.createManyAndEmit(batch)).createdCount ==
          1);
  const auto ids = DbService::client()->execSqlSync(
      "SELECT id FROM notification WHERE user_id = 31 ORDER BY id ASC");
  REQUIRE(ids.size() == 1);
  REQUIRE(drogon::sync_wait(
              service.ackDeliveries(31, {ids.front()["id"].as<int64_t>()})) ==
          1);

  const int64_t windowStart = static_cast<int64_t>(std::time(nullptr)) - 86400;
  const DeliverySummary before =
      drogon::sync_wait(service.deliverySummary(windowStart, 86400));

  const SelfTestState probe = drogon::sync_wait(service.runSelfTest());
  REQUIRE(probe.ok);

  const DeliverySummary summary =
      drogon::sync_wait(service.deliverySummary(windowStart, 86400));
  CHECK(summary.sent == before.sent);
  CHECK(summary.acked == before.acked);
  CHECK(summary.unacked == before.unacked);
  CHECK(summary.unackedOld == before.unackedOld);
}
