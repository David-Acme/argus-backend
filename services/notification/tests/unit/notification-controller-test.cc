#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/notification/controllers/notification-controller.hxx>
#include <feature/notification/controllers/notification-token-controller.hxx>
#include <feature/notification/dtos/notification-read-dto.hxx>
#include <feature/notification/dtos/notification-ack-dto.hxx>
#include <feature/notification/dtos/register-notification-token-dto.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <notification/notification-errors.hxx>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/repositories/notification-token/notification-token-index.hxx>
#include "../support/sqlite-token-index-sql.hxx"
#include <sqlite/db-service.hxx>
#include <sync/user-change-sink.hxx>
#include <shared/repositories/notification-token/notification-token-repository.hxx>
#include <feature/notification/services/token-revocation/token-revocation.hxx>
#include <sync/sync-change.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>
#include <validation/validator.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <auth/request-context.hxx>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kNotificationDb = "notification-controller-test.db";

struct RecordedAudit
{
  int64_t recordId{0};
  std::string tableName;
  std::string before;
  std::string after;
  std::vector<int64_t> users;
};

struct RecordedEmit
{
  std::vector<int64_t> users;
  Json::Value frame;
};

class RecordingSink final : public UserEmitSink
{
public:
  drogon::Task<void> publishAudit(const UserAuditInput& input) const override
  {
    std::lock_guard lock(mutex_);
    audits.push_back({input.recordId, tableNameToString(input.tableName),
                      json_util::toString(input.before),
                      json_util::toString(input.after), input.userIds});
    co_return;
  }

  drogon::Task<void> emitUsers(const UserEmitInput& input) const override
  {
    const std::scoped_lock lock(mutex_);
    emits.push_back({.users = input.userIds, .frame = input.body.toJson()});
    co_return;
  }

  mutable std::mutex mutex_;
  mutable std::vector<RecordedAudit> audits;
  mutable std::vector<RecordedEmit> emits;
};

struct Refusal
{
  int status{0};
  std::string code;
};

Refusal refusalOf(const std::function<void()>& call)
{
  try {
    call();
  }
  catch (const ResponseException& error) {
    return {.status = error.statusCode(), .code = error.errorCode()};
  }
  return {};
}

int64_t scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}

std::vector<Json::Value> listed(const NotificationRepository& repository)
{
  return drogon::sync_wait(repository.findSync({.userId = 7,
                                                .startTime = std::nullopt,
                                                .startId = std::nullopt,
                                                .endTime = std::nullopt}));
}

std::vector<int64_t> listedIds(const NotificationRepository& repository)
{
  std::vector<int64_t> ids;
  for (const auto& row : listed(repository))
    ids.push_back(row["id"].asInt64());
  std::ranges::sort(ids);
  return ids;
}

int64_t listedUnread(const NotificationRepository& repository)
{
  const auto rows = listed(repository);
  return static_cast<int64_t>(std::ranges::count_if(
      rows, [](const Json::Value& row) { return !row["isRead"].asBool(); }));
}

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

DbHandle openFile(const char* path)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open(path, &raw) != SQLITE_OK) {
    const std::string error = raw ? sqlite3_errmsg(raw) : "open failed";
    sqlite3_close_v2(raw);
    throw std::runtime_error("cannot open " + std::string(path) + ": " + error);
  }
  return {raw, sqlite3_close_v2};
}

void exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "exec failed";
    sqlite3_free(error);
    throw std::runtime_error(message + " <- " + sql);
  }
  sqlite3_free(error);
}

void seedNotificationDb(const char* path)
{
  std::remove(path);
  const auto db = openFile(path);
  exec(db.get(),
       "CREATE TABLE notification ("
       "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
       "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
       "type TEXT NOT NULL DEFAULT 'system', title TEXT NOT NULL DEFAULT '', "
       "body TEXT NOT NULL DEFAULT '', data TEXT NOT NULL DEFAULT '{}', "
       "is_read INTEGER NOT NULL DEFAULT 0 CHECK (is_read IN (0, 1)), "
       "read_at INTEGER, "
       "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  exec(db.get(),
       "CREATE TABLE notification_token ("
       "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
       "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
       "device_hash TEXT NOT NULL DEFAULT '', token TEXT NOT NULL UNIQUE, "
       "platform TEXT NOT NULL DEFAULT '', lang TEXT NOT NULL DEFAULT '', "
       "is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)), "
       "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
       "updated_at INTEGER, session_id TEXT NOT NULL DEFAULT '')");
  SqliteTokenIndexSql tokenIndex(db.get());
  const auto tokenMigration = notification_token_index::migrate(tokenIndex);
  REQUIRE(tokenMigration.changed());
  REQUIRE(tokenIndex.countRows("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'idx_notification_token_device'") == 1);
  exec(db.get(),
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
  exec(db.get(),
       "CREATE TABLE notification_selftest ("
       "id INTEGER NOT NULL PRIMARY KEY CHECK (id = 1), "
       "last_at INTEGER NOT NULL DEFAULT 0, "
       "last_ok INTEGER NOT NULL DEFAULT 0, "
       "last_ms INTEGER NOT NULL DEFAULT 0)");
  exec(db.get(),
       "CREATE INDEX IF NOT EXISTS idx_notification_token_user "
       "ON notification_token (user_id)");
  exec(db.get(),
       "INSERT INTO notification (id, user_id, type, title, body) "
       "VALUES (1, 7, 'event', 'Bell 1', 'someone rang the bell')");
  exec(db.get(),
       "INSERT INTO notification_delivery (notification_id, user_id, status, "
       "created_at, sent_at, created_ms, sent_ms) VALUES (1, 7, 'sent', "
       "1700000000, 1700000001, 1700000000000, 1700000001500)");
  exec(db.get(),
       "INSERT INTO notification (id, user_id, type, title, body) "
       "VALUES (2, 7, 'event', 'Bell 2', 'someone rang the bell')");
  exec(db.get(),
       "INSERT INTO notification_delivery (notification_id, user_id, status, "
       "created_at, sent_at, created_ms, sent_ms) VALUES (2, 7, 'sent', "
       "1700000000, 1700000001, 1700000000000, 1700000001500)");
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

Json::Value body(const drogon::HttpResponsePtr& response)
{
  const auto json = response->getJsonObject();
  REQUIRE(json);
  return *json;
}
}

TEST_CASE("notification contracts hold on the argus-notification surface")
{
  seedNotificationDb(kNotificationDb);
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kNotificationDb, "default", -1});

  const AppRunner app;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  const std::string deletedColumn =
      "SELECT COUNT(*) AS total FROM pragma_table_info('notification') "
      "WHERE name = 'deleted_at'";
  const NotificationRepository repository{};
  CHECK(scalar(deletedColumn) == 0);
  REQUIRE(repository.migrateSchema());
  CHECK(scalar(deletedColumn) == 1);
  REQUIRE(repository.migrateSchema());
  CHECK(scalar(deletedColumn) == 1);
  CHECK(scalar("SELECT COUNT(*) AS total FROM notification "
               "WHERE deleted_at IS NULL") == 2);

  RecordingSink sink;
  user_change::setNotificationSink(&sink);

  NotificationController notificationController;

  auto readReq = [&](std::vector<int64_t> ids) {
    Json::Value json;
    Json::Value idArray(Json::arrayValue);
    for (const auto id : ids)
      idArray.append(Json::Int64(id));
    Json::Value payload;
    payload["ids"] = idArray;
    auto req = drogon::HttpRequest::newHttpJsonRequest(payload);
    req->getAttributes()->insert(
        AuthContext::kJwtKey,
        JwtContext{.sub = 7, .name = "Resident", .role = UserRole::Resident, .isActive = true, .deviceHash = {}, .sessionId = {}});
    return req;
  };

  bool validationThrown = false;
  try {
    const auto dto = NotificationReadDto::fromJson(Json::Value());
    (void)dto;
  }
  catch (const ValidationException& e) {
    validationThrown = true;
    CHECK(e.statusCode() == 422);
    REQUIRE(e.errors().count("ids") == 1);
  }
  CHECK(validationThrown);

  bool oversizedThrown = false;
  try {
    Json::Value oversized(Json::objectValue);
    oversized["ids"] = Json::Value(Json::arrayValue);
    for (int64_t id = 1; id <= static_cast<int64_t>(
                                   NotificationReadDto::kMaxIdsPerRequest) + 1;
         ++id)
      oversized["ids"].append(static_cast<Json::Int64>(id));
    const auto dto = NotificationReadDto::fromJson(oversized);
    (void)dto;
  }
  catch (const ValidationException& e) {
    oversizedThrown = true;
    CHECK(e.statusCode() == 422);
    CHECK(e.errors().count("ids") == 1);
  }
  CHECK(oversizedThrown);

  const auto marked = drogon::sync_wait(notificationController.markAsRead(
      readReq({1, 2})));
  REQUIRE(marked);
  const Json::Value markedJson = body(marked);
  CHECK(markedJson["status"].asInt() == 200);
  CHECK(markedJson["errors"].isNull());
  CHECK(markedJson["info"]["updated"].asBool());

  REQUIRE(sink.audits.size() == 2);
  for (std::size_t i = 0; i < sink.audits.size(); ++i) {
    CHECK(sink.audits[i].recordId == static_cast<int64_t>(i + 1));
    CHECK(sink.audits[i].tableName == "notification");
    CHECK(sink.audits[i].users == std::vector<int64_t>{7});
    const Json::Value before = json_util::fromString(sink.audits[i].before);
    const Json::Value after = json_util::fromString(sink.audits[i].after);
    CHECK(before["isRead"].asInt() == 0);
    CHECK(after["isRead"].asInt() == 1);
    CHECK(after["readAt"].asInt64() > 0);
    const auto stored = DbService::client()->execSqlSync(
        "SELECT read_at FROM notification WHERE id = ?",
        sink.audits[i].recordId);
    REQUIRE(stored.size() == 1);
    CHECK(stored.front()["read_at"].as<int64_t>() ==
          after["readAt"].asInt64());
  }

  const auto remarkedAgain =
      drogon::sync_wait(notificationController.markAsRead(readReq({1, 2})));
  CHECK(body(remarkedAgain)["status"].asInt() == 200);
  CHECK(sink.audits.size() == 2);

  auto ackReq = [&](std::vector<int64_t> ids) {
    Json::Value idArray(Json::arrayValue);
    for (const auto id : ids)
      idArray.append(Json::Int64(id));
    Json::Value payload;
    payload["notification_ids"] = idArray;
    auto req = drogon::HttpRequest::newHttpJsonRequest(payload);
    req->getAttributes()->insert(
        AuthContext::kJwtKey,
        JwtContext{.sub = 7, .name = "Resident", .role = UserRole::Resident, .isActive = true, .deviceHash = {}, .sessionId = {}});
    return req;
  };

  bool ackValidationThrown = false;
  try {
    const auto dto = NotificationAckDto::fromJson(Json::Value());
    (void)dto;
  }
  catch (const ValidationException& e) {
    ackValidationThrown = true;
    CHECK(e.statusCode() == 422);
  }
  CHECK(ackValidationThrown);

  const auto acked =
      drogon::sync_wait(notificationController.ack(ackReq({1, 2})));
  REQUIRE(acked);
  CHECK(body(acked)["info"]["acked"].asInt64() == 2);

  const auto reacked =
      drogon::sync_wait(notificationController.ack(ackReq({1, 2})));
  CHECK(body(reacked)["info"]["acked"].asInt64() == 0);

  auto summaryInfo = [&notificationController] {
    auto summaryReq = drogon::HttpRequest::newHttpJsonRequest(Json::Value());
    summaryReq->setParameter("since", "1");
    summaryReq->getAttributes()->insert(
        AuthContext::kJwtKey,
        JwtContext{.sub = 7, .name = "Resident", .role = UserRole::Resident, .isActive = true, .deviceHash = {}, .sessionId = {}});
    const auto summary =
        drogon::sync_wait(notificationController.deliverySummary(summaryReq));
    REQUIRE(summary);
    return body(summary)["info"];
  };
  const Json::Value summaryJson = summaryInfo();
  CHECK(summaryJson["sent"].asInt64() == 2);
  CHECK(summaryJson["acked"].asInt64() == 2);
  CHECK(summaryJson["unacked"].asInt64() == 0);
  CHECK(summaryJson["latencyMsMax"].asInt64() == 1500);
  CHECK(summaryJson["probeAt"].asInt64() == 0);

  DbService::client()->execSqlSync(
      "INSERT INTO notification (id, user_id, type, title, body) "
      "VALUES (3, 7, 'event', 'Bell 3', 'someone rang the bell')");
  DbService::client()->execSqlSync(
      "INSERT INTO notification_delivery (notification_id, user_id, status, "
      "created_at, sent_at, created_ms, sent_ms) VALUES (3, 7, 'sent', "
      "1700000000, 1700000001, 1700000000000, 1700000001500)");
  CHECK(listedIds(repository) == std::vector<int64_t>{1, 2, 3});
  CHECK(listedUnread(repository) == 1);
  const Json::Value withThird = summaryInfo();
  CHECK(withThird["sent"].asInt64() == 3);
  CHECK(withThird["unacked"].asInt64() == 1);
  CHECK(withThird["unackedOld"].asInt64() == 1);

  auto removeAs = [&notificationController](int64_t userId, int64_t id) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Delete);
    req->getAttributes()->insert(
        AuthContext::kJwtKey,
        JwtContext{.sub = userId, .name = "Resident", .role = UserRole::Resident, .isActive = true, .deviceHash = {}, .sessionId = {}});
    return drogon::sync_wait(notificationController.remove(req, id));
  };
  const std::string notFound(
      NotificationErrors::NotificationNotFound.wireCode());

  const Refusal foreign =
      refusalOf([&removeAs] { static_cast<void>(removeAs(8, 3)); });
  CHECK(foreign.status == 404);
  CHECK(foreign.code == notFound);
  CHECK(scalar("SELECT COUNT(*) AS total FROM notification "
               "WHERE id = 3 AND deleted_at IS NULL") == 1);
  CHECK(sink.audits.size() == 2);
  CHECK(sink.emits.empty());

  const auto removed = removeAs(7, 3);
  REQUIRE(removed);
  CHECK(removed->statusCode() == drogon::k204NoContent);
  CHECK(body(removed)["status"].asInt() == 204);
  const int64_t deletedAt =
      scalar("SELECT COALESCE(deleted_at, 0) AS total FROM notification "
             "WHERE id = 3");
  CHECK(deletedAt > 0);

  REQUIRE(sink.audits.size() == 3);
  const RecordedAudit& deletion = sink.audits.back();
  CHECK(deletion.recordId == 3);
  CHECK(deletion.tableName == "notification");
  CHECK(deletion.users == std::vector<int64_t>{7});
  Json::Value deletionBefore = json_util::fromString(deletion.before);
  Json::Value deletionAfter = json_util::fromString(deletion.after);
  CHECK(deletionBefore["deletedAt"].isNull());
  CHECK(deletionAfter["deletedAt"].asInt64() == deletedAt);
  deletionBefore.removeMember("deletedAt");
  deletionAfter.removeMember("deletedAt");
  CHECK(deletionBefore == deletionAfter);

  REQUIRE(sink.emits.size() == 1);
  const RecordedEmit& tombstone = sink.emits.front();
  CHECK(tombstone.users == std::vector<int64_t>{7});
  CHECK(tombstone.frame["operation"].asInt() ==
        static_cast<int>(SyncOperation::Delete));
  CHECK(tombstone.frame["option"].asString() == "notification");
  CHECK(tombstone.frame["info"].getMemberNames() ==
        std::vector<std::string>{"deletedAt", "id"});
  CHECK(tombstone.frame["info"]["id"].asInt64() == 3);
  CHECK(tombstone.frame["info"]["deletedAt"].asInt64() == deletedAt);

  const Refusal twice =
      refusalOf([&removeAs] { static_cast<void>(removeAs(7, 3)); });
  CHECK(twice.status == 404);
  CHECK(twice.code == notFound);
  const Refusal missing =
      refusalOf([&removeAs] { static_cast<void>(removeAs(7, 99)); });
  CHECK(missing.status == 404);
  CHECK(missing.code == notFound);
  CHECK(sink.audits.size() == 3);
  CHECK(sink.emits.size() == 1);

  CHECK(listedIds(repository) == std::vector<int64_t>{1, 2});
  CHECK(listedUnread(repository) == 0);
  const auto last = drogon::sync_wait(repository.findLastSync(
      {.userId = 7,
       .startTime = std::nullopt,
       .startId = std::nullopt,
       .endTime = std::nullopt}));
  if (!last) {
    FAIL("expected a value in last");
    return;
  }
  CHECK((*last)["id"].asInt64() == 2);
  const auto readDeleted =
      drogon::sync_wait(notificationController.markAsRead(readReq({3})));
  CHECK(body(readDeleted)["status"].asInt() == 200);
  CHECK(sink.audits.size() == 3);
  CHECK(scalar("SELECT is_read AS total FROM notification WHERE id = 3") == 0);

  const Json::Value withoutThird = summaryInfo();
  CHECK(withoutThird["sent"].asInt64() == 2);
  CHECK(withoutThird["acked"].asInt64() == 2);
  CHECK(withoutThird["unacked"].asInt64() == 0);
  CHECK(withoutThird["unackedOld"].asInt64() == 0);

  CHECK(drogon::sync_wait(repository.purgeDeleted(deletedAt)) == 0);
  CHECK(drogon::sync_wait(repository.purgeDeleted(deletedAt + 1)) == 1);
  CHECK(scalar("SELECT COUNT(*) AS total FROM notification") == 2);
  CHECK(scalar("SELECT COUNT(*) AS total FROM notification_delivery "
               "WHERE notification_id = 3") == 0);
  CHECK(scalar("SELECT COUNT(*) AS total FROM notification_delivery") == 2);

  DbService::client()->execSqlSync(
      "INSERT INTO notification (id, user_id, type, title, body, data) VALUES "
      "(20, 7, 'event', 'Patio 1', 'motion', '{\"threadKey\":\"guard:episode:41\"}'), "
      "(21, 7, 'event', 'Patio 2', 'motion', '{\"threadKey\":\" guard:episode:41 \"}'), "
      "(22, 7, 'event', 'Garage', 'motion', '{\"threadKey\":\"guard:episode:42\"}'), "
      "(23, 8, 'event', 'Patio 1', 'motion', '{\"threadKey\":\"guard:episode:41\"}'), "
      "(24, 8, 'event', 'Fallback', 'camera off', '{\"threadKey\":\"camera:fallback:5\"}'), "
      "(25, 7, 'event', 'Numbered', 'number', '{\"threadKey\":41}'), "
      "(26, 7, 'event', 'Broken', 'broken', 'not json')");
  const std::string liveThreadRows =
      "SELECT COUNT(*) AS total FROM notification WHERE deleted_at IS NULL "
      "AND id >= 20";
  CHECK(scalar(liveThreadRows) == 7);

  auto removeThreadAs = [&notificationController](int64_t userId,
                                                  const std::string& threadKey) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Delete);
    req->getAttributes()->insert(
        AuthContext::kJwtKey,
        JwtContext{.sub = userId, .name = "Resident", .role = UserRole::Resident, .isActive = true, .deviceHash = {}, .sessionId = {}});
    return drogon::sync_wait(notificationController.removeThread(req, threadKey));
  };
  const std::size_t auditsBeforeThread = sink.audits.size();
  const std::size_t emitsBeforeThread = sink.emits.size();

  const Refusal foreignThread = refusalOf(
      [&removeThreadAs] { static_cast<void>(removeThreadAs(7, "camera:fallback:5")); });
  CHECK(foreignThread.status == 404);
  CHECK(foreignThread.code == notFound);
  const Refusal numberedThread =
      refusalOf([&removeThreadAs] { static_cast<void>(removeThreadAs(7, "41")); });
  CHECK(numberedThread.status == 404);
  CHECK(numberedThread.code == notFound);
  const Refusal blankThread =
      refusalOf([&removeThreadAs] { static_cast<void>(removeThreadAs(7, "  ")); });
  CHECK(blankThread.status == 404);
  CHECK(scalar(liveThreadRows) == 7);
  CHECK(sink.audits.size() == auditsBeforeThread);
  CHECK(sink.emits.size() == emitsBeforeThread);

  const auto threadRemoved = removeThreadAs(7, "guard:episode:41");
  REQUIRE(threadRemoved);
  CHECK(threadRemoved->statusCode() == drogon::k204NoContent);
  CHECK(scalar("SELECT COUNT(*) AS total FROM notification WHERE "
               "deleted_at IS NOT NULL AND id IN (20, 21)") == 2);
  CHECK(scalar("SELECT COUNT(*) AS total FROM notification WHERE "
               "deleted_at IS NULL AND id IN (22, 23, 24, 25, 26)") == 5);

  REQUIRE(sink.audits.size() == auditsBeforeThread + 2);
  REQUIRE(sink.emits.size() == emitsBeforeThread + 2);
  for (std::size_t i = 0; i < 2; ++i) {
    const int64_t id = 20 + static_cast<int64_t>(i);
    const RecordedAudit& audit = sink.audits[auditsBeforeThread + i];
    const RecordedEmit& frame = sink.emits[emitsBeforeThread + i];
    CHECK(audit.recordId == id);
    CHECK(audit.tableName == "notification");
    CHECK(audit.users == std::vector<int64_t>{7});
    const Json::Value before = json_util::fromString(audit.before);
    const Json::Value after = json_util::fromString(audit.after);
    CHECK(before["deletedAt"].isNull());
    CHECK(after["deletedAt"].asInt64() > 0);
    CHECK(frame.users == std::vector<int64_t>{7});
    CHECK(frame.frame["operation"].asInt() ==
          static_cast<int>(SyncOperation::Delete));
    CHECK(frame.frame["option"].asString() == "notification");
    CHECK(frame.frame["info"].getMemberNames() ==
          std::vector<std::string>{"deletedAt", "id"});
    CHECK(frame.frame["info"]["id"].asInt64() == id);
    CHECK(frame.frame["info"]["deletedAt"].asInt64() ==
          after["deletedAt"].asInt64());
  }

  const Refusal threadTwice = refusalOf(
      [&removeThreadAs] { static_cast<void>(removeThreadAs(7, "guard:episode:41")); });
  CHECK(threadTwice.status == 404);
  CHECK(threadTwice.code == notFound);
  CHECK(sink.audits.size() == auditsBeforeThread + 2);
  CHECK(sink.emits.size() == emitsBeforeThread + 2);
  CHECK(scalar("SELECT COUNT(*) AS total FROM notification WHERE id = 23 "
               "AND deleted_at IS NULL") == 1);

  NotificationTokenController tokenController;

  auto tokenReq = [&](const char* token, const std::string& deviceHash,
                      int64_t userId = 7, const std::string& sessionId = "") {
    Json::Value payload;
    payload["token"] = token;
    payload["platform"] = "android";
    payload["lang"] = "en";
    auto req = drogon::HttpRequest::newHttpJsonRequest(payload);
    req->getAttributes()->insert(
        AuthContext::kJwtKey,
        JwtContext{.sub = userId, .name = "Resident", .role = UserRole::Resident, .isActive = true, .deviceHash = {}, .sessionId = sessionId});
    req->getAttributes()->insert(AuthContext::kDeviceKey,
                                 DeviceContext{.deviceHash = deviceHash,
                                               .userAgent = "ua",
                                               .ip = "127.0.0.1",
                                               .origin = SessionOrigin::Loopback});
    return req;
  };

  const auto registered = drogon::sync_wait(
      tokenController.registerToken(tokenReq("token-a", "device-a")));
  REQUIRE(registered);
  const Json::Value registeredJson = body(registered);
  CHECK(registeredJson["status"].asInt() == 200);
  CHECK(registeredJson["errors"].isNull());
  CHECK(registeredJson["info"]["registered"].asBool());

  const NotificationTokenRepository tokenRepository;
  const auto tokens =
      drogon::sync_wait(tokenRepository.findByUser(7));
  REQUIRE(tokens.size() == 1);
  CHECK(tokens.front().token == "token-a");
  CHECK(tokens.front().deviceHash == "device-a");
  CHECK(tokens.front().platform == "android");
  CHECK(tokens.front().lang == "en");

  const auto rotated = drogon::sync_wait(
      tokenController.registerToken(tokenReq("token-b", "device-a")));
  CHECK(body(rotated)["status"].asInt() == 200);
  const auto rotatedTokens =
      drogon::sync_wait(tokenRepository.findByUser(7));
  REQUIRE(rotatedTokens.size() == 1);
  CHECK(rotatedTokens.front().token == "token-b");

  const auto reassigned = drogon::sync_wait(
      tokenController.registerToken(tokenReq("token-b", "device-b", 8)));
  CHECK(body(reassigned)["status"].asInt() == 200);
  CHECK(drogon::sync_wait(tokenRepository.findByUser(7)).empty());
  const auto newHolder = drogon::sync_wait(tokenRepository.findByUser(8));
  REQUIRE(newHolder.size() == 1);
  CHECK(newHolder.front().token == "token-b");
  CHECK(newHolder.front().deviceHash == "device-b");

  const auto again = drogon::sync_wait(
      tokenController.registerToken(tokenReq("token-b", "device-b", 8)));
  CHECK(body(again)["status"].asInt() == 200);
  CHECK(drogon::sync_wait(tokenRepository.findByUser(8)).size() == 1);

  drogon::sync_wait(tokenController.registerToken(
      tokenReq("token-phone", "device-phone", 9, "session-phone")));
  drogon::sync_wait(tokenController.registerToken(
      tokenReq("token-tablet", "device-tablet", 9, "session-tablet")));
  drogon::sync_wait(tokenController.registerToken(
      tokenReq("token-legacy", "device-legacy", 9)));
  REQUIRE(drogon::sync_wait(tokenRepository.findByUser(9)).size() == 3);

  Json::Value revoked(Json::objectValue);
  revoked[sync_change::kActionField] = sync_change::kActionDisconnectSession;
  revoked[sync_change::kUserField] = 9;
  revoked[sync_change::kSessionField] = "session-phone";
  const auto sessionRevocation = token_revocation::fromSessionChange(revoked);
  if (!sessionRevocation) {
    FAIL("expected a value in sessionRevocation");
    return;
  }
  const TokenRevocationService revocations;
  CHECK(drogon::sync_wait(revocations.revoke(*sessionRevocation)) == 1);
  CHECK(drogon::sync_wait(revocations.revoke(*sessionRevocation)) == 0);
  const auto afterSession = drogon::sync_wait(tokenRepository.findByUser(9));
  REQUIRE(afterSession.size() == 2);
  for (const auto& left : afterSession)
    CHECK(left.token != "token-phone");
  CHECK(drogon::sync_wait(revocations.revoke({.userId = 9, .sessionId = ""}))
            == 2);
  CHECK(drogon::sync_wait(tokenRepository.findByUser(9)).empty());
  CHECK(drogon::sync_wait(tokenRepository.findByUser(8)).size() == 1);

  Json::Value otherAction = revoked;
  otherAction[sync_change::kActionField] = sync_change::kActionEmit;
  CHECK_FALSE(token_revocation::fromSessionChange(otherAction));
  Json::Value noSession = revoked;
  noSession[sync_change::kSessionField] = "";
  CHECK_FALSE(token_revocation::fromSessionChange(noSession));

  Json::Value disabled(Json::objectValue);
  disabled[sync_change::kKindField] = sync_change::kKindIdentity;
  disabled[sync_change::kTableField] = tableNameToString(TableName::User);
  disabled[sync_change::kRecordIdField] = 8;
  disabled[sync_change::kDeletedField] = false;
  disabled[sync_change::kRowField]["isActive"] = false;
  const auto accountRevocation = token_revocation::fromIdentityChange(disabled);
  if (!accountRevocation) {
    FAIL("expected a value in accountRevocation");
    return;
  }
  CHECK(accountRevocation->userId == 8);
  CHECK(accountRevocation->sessionId.empty());
  CHECK(drogon::sync_wait(revocations.revoke(*accountRevocation)) == 1);
  CHECK(drogon::sync_wait(tokenRepository.findByUser(8)).empty());

  Json::Value renamed = disabled;
  renamed[sync_change::kRowField]["isActive"] = true;
  CHECK_FALSE(token_revocation::fromIdentityChange(renamed));
  Json::Value deleted = renamed;
  deleted[sync_change::kDeletedField] = true;
  CHECK(token_revocation::fromIdentityChange(deleted).has_value());
  Json::Value person = disabled;
  person[sync_change::kTableField] = tableNameToString(TableName::Person);
  CHECK_FALSE(token_revocation::fromIdentityChange(person));

  user_change::setNotificationSink(nullptr);

  std::remove(kNotificationDb);
  std::remove((std::string(kNotificationDb) + "-wal").c_str());
  std::remove((std::string(kNotificationDb) + "-shm").c_str());
  std::remove("notification-controller-test.db-wal");
  std::remove("notification-controller-test.db-shm");
}
