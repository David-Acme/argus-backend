#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <notification/notification-delivery-sink.hxx>
#include <sync/user-change-sink.hxx>
#include <shared/services/notification/delivery-page.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
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

void createTables()
{
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
      "claimed_at INTEGER NOT NULL DEFAULT 0, "
      "UNIQUE (notification_id))");
  client->execSqlSync(
      "CREATE TABLE notification_selftest ("
      "id INTEGER NOT NULL PRIMARY KEY CHECK (id = 1), "
      "last_at INTEGER NOT NULL DEFAULT 0, "
      "last_ok INTEGER NOT NULL DEFAULT 0, "
      "last_ms INTEGER NOT NULL DEFAULT 0)");
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
  bool ensureStream() const override { return streamOk; }

  bool publish(const NotificationDeliveryEvent& event) const override
  {
    published.push_back(event);
    return armed;
  }

  mutable std::vector<NotificationDeliveryEvent> published;
  mutable bool armed{false};
  mutable bool streamOk{true};
};

class SilentChangeSink final : public AuditSink
{
public:
  drogon::Task<void> publishAudit(const UserAuditInput&) const override
  {
    co_return;
  }
};
}

TEST_CASE("durable delivery keeps intents pending until the broker stores them")
{
  const TempDb db("notification-delivery-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  const AppRunner app;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  createTables();

  const SilentChangeSink changeSink;
  user_change::setNotificationSink(&changeSink);
  auto deliverySink = std::make_shared<RecordingDeliverySink>();

  NotificationBatchInput batch;
  batch.userIds = {1, 2};
  batch.notification.type = "camera";
  batch.notification.title = "Front door";
  batch.notification.body = "Person detected";
  batch.commandId = "delivery-durable";
  const NotificationService service(
      {.deliverySink = deliverySink, .pushSink = {}, .pushRequired = false});
  const auto outcome = drogon::sync_wait(service.createManyAndEmit(batch));
  CHECK_FALSE(outcome.duplicate);
  CHECK(outcome.createdCount == 2);

  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification") == 2);
  CHECK(deliverySink->published.size() == 1);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                    "WHERE status = 'pending'") == 2);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                    "WHERE status = 'sent'") == 0);

  const auto duplicate = drogon::sync_wait(service.createManyAndEmit(batch));
  CHECK(duplicate.duplicate);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification") == 2);
  REQUIRE(deliverySink->published.size() == 2);
  CHECK(deliverySink->published[1].deliveryId ==
        deliverySink->published[0].deliveryId);

  deliverySink->armed = true;
  CHECK(drogon::sync_wait(service.deliverPending()) ==
        DeliverPendingOutcome::Settled);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                    "WHERE status = 'pending'") == 0);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                    "WHERE status = 'sent'") == 2);
  REQUIRE(deliverySink->published.size() == 4);
  const auto& settled = deliverySink->published;
  CHECK(settled[2].deliveryId == settled[0].deliveryId);
  CHECK(settled[2].notificationId == settled[0].notificationId);
  CHECK(settled[2].userId == settled[0].userId);
  CHECK(settled[3].deliveryId > settled[2].deliveryId);

  {
    const NotificationService unsinked;
    CHECK_FALSE(unsinked.hasDeliverySink());
    NotificationBatchInput batch;
    batch.userIds = {3};
    batch.notification.type = "camera";
    batch.notification.title = "Back door";
    batch.notification.body = "Person detected";
    batch.commandId = "delivery-unsinked";
    bool threw = false;
    try {
      drogon::sync_wait(unsinked.createManyAndEmit(batch));
    }
    catch (const std::exception&) {
      threw = true;
    }
    CHECK_FALSE(threw);
    CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification") == 3);
    CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                      "WHERE status = 'pending'") == 1);
    CHECK(drogon::sync_wait(unsinked.deliverPending()) ==
          DeliverPendingOutcome::NoSinkInstalled);
    CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                      "WHERE status = 'pending'") == 1);
  }

  {
    deliverySink->streamOk = false;
    const size_t publishedBefore = deliverySink->published.size();
    CHECK(drogon::sync_wait(service.deliverPending()) ==
          DeliverPendingOutcome::StreamUnavailable);
    CHECK(deliverySink->published.size() == publishedBefore);
    CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                      "WHERE status = 'pending'") == 1);
    deliverySink->streamOk = true;
  }

  {
    NotificationRepository repository;
    const auto sent =
        DbService::client()->execSqlSync("SELECT id AS total FROM "
                                         "notification_delivery WHERE status "
                                         "= 'sent' LIMIT 1");
    REQUIRE(sent.size() == 1);
    CHECK(drogon::sync_wait(repository.markDelivered(
              {.deliveryIds = {sent.front()["total"].as<int64_t>()},
               .at = 1})) == 0);
  }

  {
    NotificationRepository repository;
    const auto first = drogon::sync_wait(
        repository.claimPending({.limit = 10, .now = 5000, .leaseS = 30}));
    REQUIRE(first.size() == 1);
    CHECK(first.front().title == "Back door");
    CHECK(drogon::sync_wait(repository.claimPending(
                                {.limit = 10, .now = 5000, .leaseS = 30}))
              .empty());
    CHECK(drogon::sync_wait(repository.claimPending(
                                {.limit = 10, .now = 5029, .leaseS = 30}))
              .empty());
    CHECK(drogon::sync_wait(repository.claimPending(
                                {.limit = 10, .now = 5030, .leaseS = 30}))
              .size() == 1);
    drogon::sync_wait(repository.releaseClaims({first.front().deliveryId}));
    CHECK(drogon::sync_wait(repository.claimPending(
                                {.limit = 10, .now = 5031, .leaseS = 30}))
              .size() == 1);
    drogon::sync_wait(repository.releaseClaims({first.front().deliveryId}));
  }

  {
    CHECK(deliverySink.use_count() >= 2);
    deliverySink.reset();
    CHECK(drogon::sync_wait(service.deliverPending()) ==
          DeliverPendingOutcome::Settled);
    CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                      "WHERE status = 'pending'") == 0);
  }

  user_change::setNotificationSink(nullptr);
}

namespace
{
class ScriptedDeliverySink final : public NotificationDeliverySink
{
public:
  bool ensureStream() const override { return true; }

  bool publish(const NotificationDeliveryEvent& event) const override
  {
    attempts.push_back(event.deliveryId);
    if (onPublish)
      onPublish();
    return event.deliveryId != refuse;
  }

  mutable std::vector<int64_t> attempts;
  int64_t refuse{0};
  std::function<void()> onPublish;
};

class CountingPushSink final : public push_intent::PushIntentSink
{
public:
  void publish(const PushIntent& intent) const override
  {
    pushed.push_back(intent.notificationId);
  }

  mutable std::vector<int64_t> pushed;
};

std::vector<NotificationDeliveryRow> rows(int count)
{
  std::vector<NotificationDeliveryRow> pending;
  for (int id = 1; id <= count; ++id)
    pending.push_back({.deliveryId = id,
                       .notificationId = 100 + id,
                       .userId = 7,
                       .type = "camera",
                       .title = "t",
                       .body = "b",
                       .data = Json::Value(Json::objectValue),
                       .createdAt = 1});
  return pending;
}
}

TEST_CASE("a page stops at the first refusal and pushes nothing it did not store")
{
  auto sink = std::make_shared<ScriptedDeliverySink>();
  sink->refuse = 2;
  auto push = std::make_shared<CountingPushSink>();
  const auto start = std::chrono::steady_clock::now();
  const auto page = delivery_page::publish(
      {.pending = rows(5),
       .sink = sink,
       .pushSink = push,
       .deadline = start + delivery_page::kPublishBudget,
       .clock = [start] { return start; }});
  CHECK(sink->attempts == std::vector<int64_t>{1, 2});
  CHECK(page.sent == std::vector<int64_t>{1});
  CHECK(page.unsent == std::vector<int64_t>{2, 3, 4, 5});
  CHECK(push->pushed == std::vector<int64_t>{101});
}

TEST_CASE("a slow broker cannot hold a page past its budget, so no claim is taken twice")
{
  auto sink = std::make_shared<ScriptedDeliverySink>();
  auto clock = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  const auto deadline = *clock + delivery_page::kPublishBudget;
  sink->onPublish = [clock] { *clock += delivery_page::kPublishMaxWait; };
  const auto page = delivery_page::publish(
      {.pending = rows(delivery_page::kPageSize),
       .sink = sink,
       .pushSink = nullptr,
       .deadline = deadline,
       .clock = [clock] { return *clock; }});
  const auto budgeted = static_cast<std::size_t>(
      delivery_page::kPublishBudget / delivery_page::kPublishMaxWait);
  CHECK(page.sent.size() == budgeted);
  CHECK(page.unsent.size() ==
        static_cast<std::size_t>(delivery_page::kPageSize) - budgeted);
  CHECK(*clock - (deadline - delivery_page::kPublishBudget) <
        std::chrono::seconds(delivery_page::kClaimLeaseS));

  auto late = std::make_shared<ScriptedDeliverySink>();
  const auto past = std::chrono::steady_clock::now();
  const auto first = delivery_page::publish({.pending = rows(3),
                                             .sink = late,
                                             .pushSink = nullptr,
                                             .deadline = past,
                                             .clock = [past] { return past; }});
  CHECK(first.sent == std::vector<int64_t>{1});
  CHECK(first.unsent == std::vector<int64_t>{2, 3});
}
