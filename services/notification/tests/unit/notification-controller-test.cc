#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <errors/validation-exception.hxx>
#include <feature/notification/controllers/notification-controller.hxx>
#include <feature/notification/controllers/notification-token-controller.hxx>
#include <feature/notification/dtos/notification-read-dto.hxx>
#include <feature/notification/dtos/notification-ack-dto.hxx>
#include <feature/notification/dtos/register-notification-token-dto.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <sync/user-change-sink.hxx>
#include <feature/notification/repositories/notification-token/notification-token-repository.hxx>
#include <text/json-util.hxx>
#include <validation/validator.hxx>

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
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

class RecordingSink final : public AuditSink
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

  mutable std::mutex mutex_;
  mutable std::vector<RecordedAudit> audits;
};

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
       "device_hash TEXT NOT NULL DEFAULT '', token TEXT NOT NULL, "
       "platform TEXT NOT NULL DEFAULT '', lang TEXT NOT NULL DEFAULT '', "
       "is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)), "
       "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
       "updated_at INTEGER)");
  exec(db.get(),
       "CREATE UNIQUE INDEX IF NOT EXISTS idx_notification_token_uniq "
       "ON notification_token (user_id, device_hash)");
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

  auto summaryReq = drogon::HttpRequest::newHttpJsonRequest(Json::Value());
  summaryReq->setParameter("since", "1");
  summaryReq->getAttributes()->insert(
      AuthContext::kJwtKey,
      JwtContext{.sub = 7, .name = "Resident", .role = UserRole::Resident, .isActive = true, .deviceHash = {}, .sessionId = {}});
  const auto summary =
      drogon::sync_wait(notificationController.deliverySummary(summaryReq));
  REQUIRE(summary);
  const Json::Value summaryJson = body(summary);
  CHECK(summaryJson["info"]["sent"].asInt64() == 2);
  CHECK(summaryJson["info"]["acked"].asInt64() == 2);
  CHECK(summaryJson["info"]["unacked"].asInt64() == 0);
  CHECK(summaryJson["info"]["latencyMsMax"].asInt64() == 1500);
  CHECK(summaryJson["info"]["probeAt"].asInt64() == 0);

  NotificationTokenController tokenController;

  auto tokenReq = [&](const char* token, const std::string& deviceHash) {
    Json::Value payload;
    payload["token"] = token;
    payload["platform"] = "android";
    payload["lang"] = "en";
    auto req = drogon::HttpRequest::newHttpJsonRequest(payload);
    req->getAttributes()->insert(
        AuthContext::kJwtKey,
        JwtContext{.sub = 7, .name = "Resident", .role = UserRole::Resident, .isActive = true, .deviceHash = {}, .sessionId = {}});
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

  user_change::setNotificationSink(nullptr);

  std::remove(kNotificationDb);
  std::remove((std::string(kNotificationDb) + "-wal").c_str());
  std::remove((std::string(kNotificationDb) + "-shm").c_str());
  std::remove("notification-controller-test.db-wal");
  std::remove("notification-controller-test.db-shm");
}
