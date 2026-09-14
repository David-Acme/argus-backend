#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/contracts/notification-delivery-sink.hxx>
#include <shared/contracts/user-change-sink.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <shared/services/sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
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
      "UNIQUE (notification_id))");
}

int64_t scalarCount(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return -1;
  return rows.front()["total"].as<int64_t>();
}

// Fails closed until armed: unpublished deliveries must stay pending.
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

class SilentChangeSink final : public UserChangeSink
{
public:
  void emitUser(int64_t, const SocketEmitDto&) const override { ++emits; }
  void emitUsers(const std::vector<int64_t>&,
                 const SocketEmitDto&) const override
  {
    ++emits;
  }
  drogon::Task<void> publishAudit(const UserAuditInput&) const override
  {
    co_return;
  }

  mutable int emits{0};
};
} // namespace

TEST_CASE("durable delivery keeps intents pending until the broker stores them")
{
  const TempDb db("notification-delivery-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  std::thread runner([] { drogon::app().run(); });
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
  CHECK(deliverySink->published.size() == 2);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                    "WHERE status = 'pending'") == 2);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                    "WHERE status = 'sent'") == 0);

  const auto duplicate = drogon::sync_wait(service.createManyAndEmit(batch));
  CHECK(duplicate.duplicate);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification") == 2);
  REQUIRE(deliverySink->published.size() == 4);
  for (const auto& republished : deliverySink->published) {
    CHECK((republished.deliveryId == deliverySink->published[0].deliveryId ||
           republished.deliveryId == deliverySink->published[1].deliveryId));
  }

  deliverySink->armed = true;
  drogon::sync_wait(service.deliverPending());
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                    "WHERE status = 'pending'") == 0);
  CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                    "WHERE status = 'sent'") == 2);
  REQUIRE(deliverySink->published.size() == 6);
  const auto& settled = deliverySink->published;
  CHECK(settled[4].deliveryId == settled[0].deliveryId);
  CHECK(settled[4].notificationId == settled[0].notificationId);
  CHECK(settled[4].userId == settled[0].userId);
  CHECK(settled[5].deliveryId == settled[1].deliveryId);

  {
    const NotificationService unsinked;
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
    CHECK(threw);
    CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification") == 3);
    CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                      "WHERE status = 'pending'") == 1);
    CHECK(changeSink.emits == 0);
  }

  {
    deliverySink->streamOk = false;
    const size_t publishedBefore = deliverySink->published.size();
    drogon::sync_wait(service.deliverPending());
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
    CHECK_FALSE(drogon::sync_wait(
        repository.markDelivered(sent.front()["total"].as<int64_t>(), 1)));
  }

  {
    CHECK(deliverySink.use_count() >= 2);
    deliverySink.reset();
    drogon::sync_wait(service.deliverPending());
    CHECK(scalarCount("SELECT COUNT(*) AS total FROM notification_delivery "
                      "WHERE status = 'pending'") == 0);
  }

  user_change::setNotificationSink(nullptr);
  drogon::app().quit();
  runner.join();
}
