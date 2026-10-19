#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/services/change-sink/nats-notification-change-sink.hxx>
#include <nats/nats-subject.hxx>
#include <outbox/outbox-repository.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <sqlite/db-service.hxx>
#include <sync/user-change-sink.hxx>
#include <text/json-util.hxx>

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

class RefusingSink final : public UserEmitSink
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

  drogon::Task<void> emitUsers(const UserEmitInput& input) const override
  {
    note(input.client);
    if (refuse_)
      throw std::runtime_error("the change sink refused the emit");
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

class EmitRefusingSink final : public UserEmitSink
{
public:
  explicit EmitRefusingSink(const UserEmitSink* durable) : durable_(durable) {}

  drogon::Task<void> publishAudit(const UserAuditInput& input) const override
  {
    co_await durable_->publishAudit(input);
  }

  drogon::Task<void> emitUsers(const UserEmitInput& input) const override
  {
    if (refuseEmits_)
      throw std::runtime_error("the change sink refused the emit");
    co_await durable_->emitUsers(input);
  }

private:
  const UserEmitSink* durable_;
  bool refuseEmits_{true};
};

int64_t deletedAt(int64_t id)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT COALESCE(deleted_at, 0) AS total FROM notification WHERE id = ?",
      id);
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}

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
  return static_cast<int64_t>(inserted.insertId());
}

struct SeededRow
{
  int64_t userId{7};
  std::string title;
  std::string data;
};

int64_t seedRow(const SeededRow& row)
{
  const auto inserted = DbService::client()->execSqlSync(
      "INSERT INTO notification (user_id, type, title, body, data) "
      "VALUES (?, 'event', ?, 'motion in the yard', ?)",
      row.userId, row.title, row.data);
  return static_cast<int64_t>(inserted.insertId());
}

int64_t unreadNotifications()
{
  return scalar("SELECT COUNT(*) AS total FROM notification "
                "WHERE user_id = 7 AND is_read = 0 AND deleted_at IS NULL");
}

int64_t settledReadAt(int64_t id)
{
  return scalar("SELECT COALESCE(read_at, 0) AS total FROM notification "
                "WHERE id = " +
                std::to_string(id));
}
}

TEST_CASE("a notification read, delete or thread delete and its changes are one unit of work")
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

  const EmitRefusingSink halfway(&durableSink);
  user_change::setNotificationSink(&halfway);
  CHECK_THROWS_AS(
      drogon::sync_wait(notifications.remove({.id = second, .userId = 7})),
      std::runtime_error);
  CHECK(deletedAt(second) == 0);
  CHECK(outbox.pendingBatch(10).size() == 1);

  user_change::setNotificationSink(&durableSink);
  CHECK_FALSE(
      drogon::sync_wait(notifications.remove({.id = second, .userId = 8})));
  CHECK(deletedAt(second) == 0);
  CHECK(outbox.pendingBatch(10).size() == 1);

  REQUIRE(drogon::sync_wait(notifications.remove({.id = second, .userId = 7})));
  const int64_t removedAt = deletedAt(second);
  CHECK(removedAt > 0);
  const auto removal = outbox.pendingBatch(10);
  REQUIRE(removal.size() == 3);

  const Json::Value audit = json_util::fromString(removal[1].payload);
  CHECK(audit["kind"].asString() == "audit");
  CHECK(audit["record_id"].asInt64() == second);
  CHECK(audit["table_name"].asString() == "notification");
  CHECK(audit["changes"].getMemberNames() ==
        std::vector<std::string>{"deletedAt"});
  CHECK(audit["changes"]["deletedAt"]["previous"].isNull());
  CHECK(audit["changes"]["deletedAt"]["current"].asInt64() == removedAt);
  REQUIRE(audit["users"].size() == 1U);
  CHECK(audit["users"][0].asInt64() == 7);

  const Json::Value tombstone = json_util::fromString(removal[2].payload);
  CHECK(tombstone.getMemberNames() ==
        std::vector<std::string>{"info", "operation", "option", "users"});
  CHECK(tombstone["operation"].asInt() ==
        static_cast<int>(SyncOperation::Delete));
  CHECK(tombstone["option"].asString() == "notification");
  CHECK(tombstone["info"].getMemberNames() ==
        std::vector<std::string>{"deletedAt", "id"});
  CHECK(tombstone["info"]["id"].asInt64() == second);
  CHECK(tombstone["info"]["deletedAt"].asInt64() == removedAt);
  REQUIRE(tombstone["users"].size() == 1U);
  CHECK(tombstone["users"][0].asInt64() == 7);
  CHECK(removal[2].eventId.rfind("notification-change:", 0) == 0);
  CHECK(removal[1].subject == nats_subject::kNotificationChange);
  CHECK(removal[2].subject == nats_subject::kNotificationChange);

  CHECK_FALSE(
      drogon::sync_wait(notifications.remove({.id = second, .userId = 7})));
  CHECK(outbox.pendingBatch(10).size() == 3);

  const std::string episode = R"({"threadKey":"guard:episode:41"})";
  const int64_t ownFirst =
      seedRow({.userId = 7, .title = "Patio 1", .data = episode});
  const int64_t ownSecond =
      seedRow({.userId = 7, .title = "Patio 2", .data = episode});
  const int64_t theirs =
      seedRow({.userId = 8, .title = "Patio 1", .data = episode});
  REQUIRE(theirs > ownSecond);
  const std::size_t queued = outbox.pendingBatch(50).size();
  const NotificationThreadRef ownThread{.threadKey = "guard:episode:41",
                                        .userId = 7};
  const NotificationThreadRef strangerThread{.threadKey = "guard:episode:41",
                                             .userId = 9};

  user_change::setNotificationSink(&halfway);
  CHECK_THROWS_AS(drogon::sync_wait(notifications.removeThread(ownThread)),
                  std::runtime_error);
  CHECK(deletedAt(ownFirst) == 0);
  CHECK(deletedAt(ownSecond) == 0);
  CHECK(outbox.pendingBatch(50).size() == queued);

  user_change::setNotificationSink(&durableSink);
  CHECK_FALSE(drogon::sync_wait(notifications.removeThread(strangerThread)));
  CHECK(deletedAt(ownFirst) == 0);
  CHECK(deletedAt(theirs) == 0);
  CHECK(outbox.pendingBatch(50).size() == queued);

  REQUIRE(drogon::sync_wait(notifications.removeThread(ownThread)));
  CHECK(deletedAt(ownFirst) > 0);
  CHECK(deletedAt(ownSecond) > 0);
  CHECK(deletedAt(theirs) == 0);
  const auto threadRows = outbox.pendingBatch(50);
  REQUIRE(threadRows.size() == queued + 4);
  for (std::size_t i = 0; i < 2; ++i) {
    const int64_t id = i == 0 ? ownFirst : ownSecond;
    const Json::Value threadAudit =
        json_util::fromString(threadRows[queued + 2 * i].payload);
    const Json::Value threadTombstone =
        json_util::fromString(threadRows[queued + 2 * i + 1].payload);
    CHECK(threadAudit["kind"].asString() == "audit");
    CHECK(threadAudit["record_id"].asInt64() == id);
    CHECK(threadAudit["changes"].getMemberNames() ==
          std::vector<std::string>{"deletedAt"});
    CHECK(threadAudit["changes"]["deletedAt"]["current"].asInt64() ==
          deletedAt(id));
    CHECK(threadTombstone["operation"].asInt() ==
          static_cast<int>(SyncOperation::Delete));
    CHECK(threadTombstone["info"]["id"].asInt64() == id);
    CHECK(threadTombstone["info"]["deletedAt"].asInt64() == deletedAt(id));
    REQUIRE(threadTombstone["users"].size() == 1U);
    CHECK(threadTombstone["users"][0].asInt64() == 7);
  }

  CHECK_FALSE(drogon::sync_wait(notifications.removeThread(ownThread)));
  CHECK(outbox.pendingBatch(50).size() == queued + 4);

  const std::string readThread = R"({"threadKey":"guard:episode:53"})";
  const int64_t readFirst =
      seedRow({.userId = 7, .title = "Porch 1", .data = readThread});
  const int64_t readSecond =
      seedRow({.userId = 7, .title = "Porch 2", .data = readThread});
  const int64_t readSettled =
      seedRow({.userId = 7, .title = "Porch 3", .data = readThread});
  const int64_t readTheirs =
      seedRow({.userId = 8, .title = "Porch 1", .data = readThread});
  DbService::client()->execSqlSync(
      "UPDATE notification SET is_read = 1, read_at = 1700000000 "
      "WHERE id = ?",
      readSettled);
  const std::size_t readQueued = outbox.pendingBatch(50).size();
  const NotificationThreadRef readOwn{.threadKey = "guard:episode:53",
                                      .userId = 7};

  RefusingSink readRefusal(pooled.get());
  readRefusal.refuse(true);
  user_change::setNotificationSink(&readRefusal);
  CHECK_THROWS_AS(drogon::sync_wait(notifications.markThreadAsRead(readOwn)),
                  std::runtime_error);
  CHECK(readRefusal.calls() == 1);
  CHECK(readRefusal.transactional());
  CHECK(settledReadAt(readFirst) == 0);
  CHECK(settledReadAt(readSecond) == 0);
  CHECK(settledReadAt(readSettled) == 1700000000);
  CHECK(settledReadAt(readTheirs) == 0);
  CHECK(outbox.pendingBatch(50).size() == readQueued);

  user_change::setNotificationSink(&durableSink);
  drogon::sync_wait(notifications.markThreadAsRead(readOwn));
  CHECK(settledReadAt(readFirst) > 0);
  CHECK(settledReadAt(readSecond) > 0);
  CHECK(settledReadAt(readSettled) == 1700000000);
  CHECK(settledReadAt(readTheirs) == 0);
  const auto readRows = outbox.pendingBatch(50);
  REQUIRE(readRows.size() == readQueued + 2);
  for (std::size_t i = 0; i < 2; ++i) {
    const int64_t id = i == 0 ? readFirst : readSecond;
    const Json::Value readAudit =
        json_util::fromString(readRows[readQueued + i].payload);
    CHECK(readAudit["kind"].asString() == "audit");
    CHECK(readAudit["record_id"].asInt64() == id);
    CHECK(readAudit["table_name"].asString() == "notification");
    CHECK(readAudit["changes"].getMemberNames().size() == 2U);
    CHECK(readAudit["changes"]["isRead"]["previous"].asInt() == 0);
    CHECK(readAudit["changes"]["isRead"]["current"].asInt() == 1);
    CHECK(readAudit["changes"]["readAt"]["previous"].isNull());
    CHECK(readAudit["changes"]["readAt"]["current"].asInt64() ==
          settledReadAt(id));
    REQUIRE(readAudit["users"].size() == 1U);
    CHECK(readAudit["users"][0].asInt64() == 7);
  }

  const std::size_t readSettledQueued = outbox.pendingBatch(50).size();
  drogon::sync_wait(notifications.markThreadAsRead(
      {.threadKey = "guard:episode:53", .userId = 9}));
  CHECK(outbox.pendingBatch(50).size() == readSettledQueued);
  drogon::sync_wait(notifications.markThreadAsRead(readOwn));
  CHECK(outbox.pendingBatch(50).size() == readSettledQueued);

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
