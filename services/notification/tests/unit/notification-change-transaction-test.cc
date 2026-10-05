#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/services/change-sink/nats-notification-change-sink.hxx>
#include <outbox/outbox-repository.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <sqlite/db-service.hxx>
#include <sync/user-change-sink.hxx>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_NOTIFICATION_SCHEMA
#error "ARGUS_NOTIFICATION_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kTransactionDb =
    "notification-change-transaction-test.db";

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

class RefusingSink final : public AuditSink
{
public:
  explicit RefusingSink(const drogon::orm::DbClient* pooled) : pooled_(pooled)
  {
  }

  drogon::Task<void> publishAudit(const UserAuditInput& input) const override
  {
    note(input.client);
    if (refuse_)
      throw std::runtime_error("the change sink refused the audit");
    co_return;
  }

  void refuse(bool value) const { refuse_ = value; }
  [[nodiscard]] int calls() const { return calls_; }
  [[nodiscard]] bool sawClient() const { return sawClient_; }
  [[nodiscard]] bool transactional() const { return transactional_; }

private:
  void note(const drogon::orm::DbClient* client) const
  {
    ++calls_;
    sawClient_ = client != nullptr;
    transactional_ = sawClient_ && client != pooled_;
  }

  mutable bool refuse_{false};
  mutable int calls_{0};
  mutable bool sawClient_{false};
  mutable bool transactional_{false};
  const drogon::orm::DbClient* pooled_;
};

int64_t scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return -1;
  return rows.front()["total"].as<int64_t>();
}

int64_t seedNotification(const std::string& title)
{
  const auto inserted = DbService::client()->execSqlSync(
      "INSERT INTO notification (user_id, type, title, body) "
      "VALUES (7, 'event', ?, 'someone rang the bell')",
      title);
  return inserted.insertId();
}

int64_t unreadNotifications()
{
  return scalar("SELECT COUNT(*) AS total FROM notification "
                "WHERE user_id = 7 AND is_read = 0");
}

int64_t settledReadAt(int64_t id)
{
  return scalar("SELECT COALESCE(read_at, 0) AS total FROM notification "
                "WHERE id = " +
                std::to_string(id));
}
}

TEST_CASE("a notification read and its change are one unit of work")
{
  std::remove(kTransactionDb);
  std::remove((std::string(kTransactionDb) + "-wal").c_str());
  std::remove((std::string(kTransactionDb) + "-shm").c_str());
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                 .filename = kTransactionDb,
                                 .name = "default",
                                 .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_NOTIFICATION_SCHEMA));

  const int64_t first = seedNotification("Bell 1");
  const int64_t second = seedNotification("Bell 2");
  const int64_t third = seedNotification("Bell 3");
  REQUIRE(first > 0);
  REQUIRE(second > first);
  REQUIRE(third > second);

  NotificationService notifications;
  const auto pooled = DbService::client();

  RefusingSink sink(pooled.get());
  user_change::setNotificationSink(&sink);

  sink.refuse(true);
  CHECK_THROWS_AS(
      drogon::sync_wait(notifications.markAsRead(7, {first, second})),
      std::runtime_error);
  CHECK(unreadNotifications() == 3);
  CHECK(settledReadAt(first) == 0);
  CHECK(settledReadAt(second) == 0);

  sink.refuse(false);
  const int beforeRead = sink.calls();
  drogon::sync_wait(notifications.markAsRead(7, {first, second}));
  CHECK(sink.calls() == beforeRead + 2);
  CHECK(sink.sawClient());
  CHECK(sink.transactional());
  CHECK(unreadNotifications() == 1);
  CHECK(settledReadAt(first) > 0);
  CHECK(settledReadAt(second) > 0);

  drogon::sync_wait(notifications.markAsRead(7, {first, second}));
  CHECK(sink.calls() == beforeRead + 2);
  CHECK(unreadNotifications() == 1);

  NatsNotificationChangeSink durableSink(
      nullptr, NatsNotificationChangeSink::Config{.retryMs = 20,
                                                  .publishSubject = {},
                                                  .streamName = {}});
  user_change::setNotificationSink(&durableSink);

  const auto outbox = NatsNotificationChangeSink::repository();
  drogon::sync_wait(notifications.markAsRead(7, {third}));
  CHECK(unreadNotifications() == 0);
  const auto pending = outbox.pendingBatch(10);
  REQUIRE(pending.size() == 1);
  CHECK(pending.front().eventId.rfind("notification-change:", 0) == 0);
  CHECK(pending.front().payload.find(std::to_string(third)) !=
        std::string::npos);
  CHECK(pending.front().attempts == 0);

  DbService::client()->execSqlSync("DROP TABLE change_outbox");

  const int64_t orphan = seedNotification("Bell 4");
  REQUIRE(orphan > third);
  CHECK_THROWS(drogon::sync_wait(notifications.markAsRead(7, {orphan})));
  CHECK(unreadNotifications() == 1);
  CHECK(settledReadAt(orphan) == 0);

  user_change::setNotificationSink(nullptr);
  std::remove(kTransactionDb);
  std::remove((std::string(kTransactionDb) + "-wal").c_str());
  std::remove((std::string(kTransactionDb) + "-shm").c_str());
}
