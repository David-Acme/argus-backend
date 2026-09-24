#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <feature/fanout/services/change-feed-consumer.hxx>
#include <nats/nats-subject.hxx>
#include <sqlite/db-service.hxx>
#include <sync/module-audit-event.hxx>
#include <sync/sync-change.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <sync/user-action-event.hxx>
#include <sync/user-audit-event.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#ifndef ARGUS_SYNC_SCHEMA_PATH
#error "ARGUS_SYNC_SCHEMA_PATH must point at the sync schema.sql"
#endif

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

Json::Value record(int64_t id, const std::string& name)
{
  Json::Value value;
  value["id"] = static_cast<Json::Int64>(id);
  value["name"] = name;
  return value;
}

Json::Value moduleAudit(int64_t recordId, TableName tableName)
{
  ModuleAuditEvent event;
  event.recordId = recordId;
  event.tableName = tableName;
  event.changes =
      JsonDiff::createFlatDiff(record(recordId, "before"),
                               record(recordId, "after"));
  event.createUserId = 42;
  event.eventTimestamp = 1735689600000;
  return event.toJson();
}

Json::Value moduleAuditStep(int64_t recordId, int step)
{
  ModuleAuditEvent event;
  event.recordId = recordId;
  event.tableName = TableName::Camera;
  event.changes = JsonDiff::createFlatDiff(
      record(recordId, "v" + std::to_string(step - 1)),
      record(recordId, "v" + std::to_string(step)));
  event.createUserId = 42;
  event.eventTimestamp = 1735689600000;
  return event.toJson();
}

Json::Value userAudit(int64_t recordId, const std::vector<int64_t>& users)
{
  UserAuditEvent event;
  event.recordId = recordId;
  event.tableName = TableName::Project;
  event.changes =
      JsonDiff::createFlatDiff(record(recordId, "before"),
                               record(recordId, "after"));
  event.users = users;
  event.eventTimestamp = 1735689600000;
  return event.toJson();
}

Json::Value actionJournal(int64_t recordId)
{
  UserActionEvent event;
  event.userId = 42;
  event.recordId = recordId;
  event.tableName = TableName::User;
  event.action = UserAction::Read;
  event.oldData = Json::Value(Json::objectValue);
  event.newData = Json::Value(Json::objectValue);
  event.ipAddress = "10.0.0.1";
  return event.toJson();
}

Json::Value catalogChange()
{
  Json::Value json(Json::objectValue);
  json[sync_change::kKindField] = sync_change::kKindIdentity;
  json["table"] = "person";
  json["id"] = static_cast<Json::Int64>(1);
  json["deleted"] = false;
  json["row"] = record(1, "Ada");
  return json;
}

Json::Value cameraEmit()
{
  Json::Value json(Json::objectValue);
  json["operation"] = static_cast<int>(SyncOperation::Add);
  json["option"] = "camera";
  json["info"] = record(7, "Front door");
  return json;
}
}

TEST_CASE("every producer stream carries the durable the change feed holds")
{
  const auto& feeds = change_feed::defaults();
  REQUIRE(feeds.size() == 5);

  CHECK(feeds[0].stream == std::string(nats_subject::kCameraStream));
  CHECK(feeds[0].subject == std::string(nats_subject::kCameraChange));
  CHECK(feeds[0].durable == "argus-sync-camera");

  CHECK(feeds[1].stream == std::string(nats_subject::kNotificationChangeStream));
  CHECK(feeds[1].subject == std::string(nats_subject::kNotificationChange));
  CHECK(feeds[1].durable == "argus-sync-notification");

  CHECK(feeds[2].stream ==
        std::string(nats_subject::kProductivityChangeStream));
  CHECK(feeds[2].subject == std::string(nats_subject::kProductivityChange));
  CHECK(feeds[2].durable == "argus-sync-productivity");

  CHECK(feeds[3].stream == std::string(nats_subject::kIdentityChangeStream));
  CHECK(feeds[3].subject == std::string(nats_subject::kIdentityChange));
  CHECK(feeds[3].durable == "argus-sync-identity");

  CHECK(feeds[4].stream == std::string(nats_subject::kIdentityChangeStream));
  CHECK(feeds[4].subject == std::string(nats_subject::kIdentityUserAction));
  CHECK(feeds[4].durable == "argus-sync-identity-action");

  std::unordered_set<std::string> durables;
  for (const auto& feed : feeds) {
    CHECK(nats_subject::isValidSubject(feed.subject,
                                       nats_subject::SubjectKind::Subscribe));
    CHECK_FALSE(feed.stream.empty());
    CHECK(durables.insert(feed.durable).second);
  }
}

TEST_CASE("the change feed applies, routes and settles every change subject")
{
  const TempDb db("change-feed-consumer-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  AuditFanOut auditFanOut;
  REQUIRE(auditFanOut.migrateLegacySchema());
  DbService::client()->execSqlSync(
      "CREATE TABLE user (id INTEGER PRIMARY KEY)");
  DbService::client()->execSqlSync("INSERT INTO user (id) VALUES (42), (43)");
  DbService::client()->execSqlSync(
      "CREATE TABLE user_action_log ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "record_id INTEGER NOT NULL, table_name TEXT NOT NULL, "
      "action TEXT NOT NULL CHECK (action IN "
      "('create', 'read', 'update', 'delete')), "
      "old_data TEXT NOT NULL DEFAULT '{}', "
      "new_data TEXT NOT NULL DEFAULT '{}', "
      "ip_address TEXT NOT NULL DEFAULT '', "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  DbService::client()->execSqlSync(
      "INSERT INTO user_action_log (user_id, record_id, table_name, action) "
      "VALUES (42, 1, 'user', 'read')");
  REQUIRE(auditFanOut.migrateLegacySchema());
  REQUIRE(DbService::runScriptFile(ARGUS_SYNC_SCHEMA_PATH));
  REQUIRE(auditFanOut.migrateLegacySchema());
  CHECK(scalar("SELECT COUNT(*) FROM pragma_table_info('user_action_log') "
               "WHERE name = 'msg_id'") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM sqlite_master "
               "WHERE name = 'idx_user_action_log_msg_id'") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log WHERE msg_id = ''") ==
        "1");
  DbService::client()->execSqlSync("DELETE FROM user_action_log");

  ChangeFeedConsumer consumer({.bus = nullptr, .auditFanOut = &auditFanOut},
                              ChangeFeedConsumer::Config{});

  CHECK(drogon::sync_wait(consumer.handle(
            {.subject = nats_subject::kCameraChange,
             .msgId = "camera-change:7",
             .body = json_util::toString(
                 moduleAudit(7, TableName::Camera))})) ==
        DurableDisposition::Ack);
  CHECK(scalar("SELECT COUNT(*) FROM audit_log") == "1");
  CHECK(scalar("SELECT record_id FROM audit_log") == "7");

  CHECK(drogon::sync_wait(consumer.handle(
            {.subject = nats_subject::kCameraChange,
             .msgId = "camera-change:7",
             .body = json_util::toString(
                 moduleAudit(7, TableName::Camera))})) ==
        DurableDisposition::Ack);
  CHECK(scalar("SELECT COUNT(*) FROM audit_log") == "1");
  CHECK(scalar("SELECT id FROM audit_log") == "2");

  CHECK(drogon::sync_wait(consumer.handle(
            {.subject = nats_subject::kProductivityChange,
             .msgId = "productivity-change:9",
             .body = json_util::toString(userAudit(9, {42, 43}))})) ==
        DurableDisposition::Ack);
  CHECK(scalar("SELECT COUNT(*) FROM user_audit_log") == "2");
  CHECK(scalar("SELECT COUNT(DISTINCT user_id) FROM user_audit_log") == "2");

  CHECK(drogon::sync_wait(consumer.handle(
            {.subject = nats_subject::kCameraChange,
             .msgId = "camera-change:emit",
             .body = json_util::toString(cameraEmit())})) ==
        DurableDisposition::Ack);
  CHECK(scalar("SELECT COUNT(*) FROM audit_log") == "1");

  CHECK(drogon::sync_wait(consumer.handle(
            {.subject = nats_subject::kIdentityChange,
             .msgId = "identity-change:1",
             .body = json_util::toString(catalogChange())})) ==
        DurableDisposition::Ack);
  CHECK(scalar("SELECT COUNT(*) FROM audit_log") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log") == "0");

  CHECK(drogon::sync_wait(consumer.handle(
            {.subject = nats_subject::kIdentityUserAction,
             .msgId = "identity-action:1",
             .body = json_util::toString(actionJournal(5))})) ==
        DurableDisposition::Ack);
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log") == "1");

  CHECK(drogon::sync_wait(consumer.handle(
            {.subject = nats_subject::kIdentityUserAction,
             .msgId = "identity-action:1",
             .body = json_util::toString(actionJournal(5))})) ==
        DurableDisposition::Ack);
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log") == "1");

  CHECK(drogon::sync_wait(consumer.handle(
            {.subject = nats_subject::kIdentityUserAction,
             .msgId = "identity-action:2",
             .body = json_util::toString(actionJournal(6))})) ==
        DurableDisposition::Ack);
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log") == "2");
  CHECK(scalar("SELECT msg_id FROM user_action_log WHERE record_id = 6") ==
        "identity-action:2");

  CHECK(drogon::sync_wait(consumer.handle(
            {.subject = nats_subject::kIdentityUserAction,
             .msgId = "identity-action:3",
             .body = json_util::toString(
                 moduleAudit(7, TableName::Camera))})) ==
        DurableDisposition::Term);
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log") == "2");

  CHECK(drogon::sync_wait(consumer.handle(
            {.subject = nats_subject::kNotificationChange,
             .msgId = "notification-change:malformed",
             .body = "{\"users\":[]}"})) == DurableDisposition::Term);
  CHECK(drogon::sync_wait(consumer.handle(
            {.subject = nats_subject::kNotificationChange,
             .msgId = "notification-change:garbage",
             .body = "not-json"})) == DurableDisposition::Term);
  CHECK(scalar("SELECT COUNT(*) FROM audit_log") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM user_audit_log") == "2");

  std::atomic<int> acked{0};
  std::atomic<int> refused{0};
  const auto burst = durable_delivery::handler(
      "Change feed burst",
      [&consumer](const durable_delivery::Payload& payload) {
        return consumer.handle(payload);
      });
  constexpr int kBurst = 20;
  for (int step = 1; step <= kBurst; ++step) {
    const std::string body = json_util::toString(moduleAuditStep(11, step));
    const std::string msgId = "camera-change:burst-" + std::to_string(step);
    burst({.subject = nats_subject::kCameraChange,
           .payload = body,
           .msgId = msgId,
           .delivered = 1},
          NatsBus::DurableSettlement{.ack = [&acked] { ++acked; },
                                     .nak = [&refused] { ++refused; },
                                     .term = [&refused] { ++refused; }});
  }
  for (int i = 0; i < 500 && acked.load() + refused.load() < kBurst; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(acked.load() == kBurst);
  CHECK(refused.load() == 0);
  CHECK(scalar("SELECT COUNT(*) FROM audit_log WHERE record_id = 11") == "1");
  CHECK(scalar("SELECT json_extract(changes, '$.name.previous') FROM "
               "audit_log WHERE record_id = 11") == "v0");
  CHECK(scalar("SELECT json_extract(changes, '$.name.current') FROM "
               "audit_log WHERE record_id = 11") ==
        "v" + std::to_string(kBurst));
}
