#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/app-config.hxx>
#include <drogon/drogon.h>
#include <feature/api/notification/controllers/notification-controller.hxx>
#include <feature/api/notification/controllers/notification-token-controller.hxx>
#include <feature/api/notification/dtos/notification-read-dto.hxx>
#include <feature/api/notification/dtos/register-notification-token-dto.hxx>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/contracts/user-change-sink.hxx>
#include <shared/repositories/notification-token/notification-token-repository.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/validation/validator.hxx>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kNotificationDb = "notification-controller-test.db";

struct RecordedEmit
{
  int operation{0};
  std::string option;
  Json::Value body;
  std::vector<int64_t> users;
};

struct RecordedAudit
{
  int64_t recordId{0};
  std::string tableName;
  std::string before;
  std::string after;
  std::vector<int64_t> users;
};

// Records the emits and audit diffs the feature services hand to the funnel.
class RecordingSink final : public UserChangeSink
{
public:
  void emitUser(int64_t userId, const SocketEmitDto& body) const override
  {
    emitUsers({userId}, body);
  }

  void emitUsers(const std::vector<int64_t>& userIds,
                 const SocketEmitDto& body) const override
  {
    std::lock_guard lock(mutex_);
    emits.push_back({static_cast<int>(body.operation),
                     tableNameToString(body.option), body.obj, userIds});
  }

  drogon::Task<void> publishAudit(const UserAuditInput& input) const override
  {
    std::lock_guard lock(mutex_);
    audits.push_back({input.recordId, tableNameToString(input.tableName),
                      json_util::toString(input.before),
                      json_util::toString(input.after), input.userIds});
    co_return;
  }

  mutable std::mutex mutex_;
  mutable std::vector<RecordedEmit> emits;
  mutable std::vector<RecordedAudit> audits;
};

// Seeds both notification tables and the notification_token indexes.
void seedNotificationDb(const char* path)
{
  std::remove(path);
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
                                              1);
  client->execSqlSync(
      "CREATE TABLE notification ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "type TEXT NOT NULL DEFAULT 'system', title TEXT NOT NULL DEFAULT '', "
      "body TEXT NOT NULL DEFAULT '', data TEXT NOT NULL DEFAULT '{}', "
      "is_read INTEGER NOT NULL DEFAULT 0 CHECK (is_read IN (0, 1)), "
      "read_at INTEGER, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  client->execSqlSync(
      "CREATE TABLE notification_token ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "device_hash TEXT NOT NULL DEFAULT '', token TEXT NOT NULL, "
      "platform TEXT NOT NULL DEFAULT '', lang TEXT NOT NULL DEFAULT '', "
      "is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)), "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER)");
  client->execSqlSync(
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_notification_token_uniq "
      "ON notification_token (user_id, device_hash)");
  client->execSqlSync(
      "CREATE INDEX IF NOT EXISTS idx_notification_token_user "
      "ON notification_token (user_id)");
  for (const auto id : {1, 2}) {
    client->execSqlSync(
        "INSERT INTO notification (id, user_id, type, title, body) "
        "VALUES (?, 7, 'event', ?, 'someone rang the bell')",
        id, ("Bell " + std::to_string(id)));
  }
}

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
} // namespace

TEST_CASE("notification contracts hold on the argus-notification surface")
{
  seedNotificationDb(kNotificationDb);
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kNotificationDb, "default", -1});

  std::thread runner([] { drogon::app().run(); });
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
        AppConfig::JWT_CTX_KEY,
        JwtContext{7, "Resident", UserRole::Resident, true, {}});
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
  CHECK(sink.emits.empty());

  const auto remarkedAgain =
      drogon::sync_wait(notificationController.markAsRead(readReq({1, 2})));
  CHECK(body(remarkedAgain)["status"].asInt() == 200);
  CHECK(sink.audits.size() == 2);

  NotificationTokenController tokenController;

  auto tokenReq = [&](const char* token, const std::string& deviceHash) {
    Json::Value payload;
    payload["token"] = token;
    payload["platform"] = "android";
    payload["lang"] = "en";
    auto req = drogon::HttpRequest::newHttpJsonRequest(payload);
    req->getAttributes()->insert(
        AppConfig::JWT_CTX_KEY,
        JwtContext{7, "Resident", UserRole::Resident, true, {}});
    req->getAttributes()->insert(AppConfig::DEVICE_CTX_KEY,
                                 DeviceContext{deviceHash, "ua", "127.0.0.1"});
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

  drogon::app().quit();
  runner.join();

  std::remove(kNotificationDb);
  std::remove("notification-controller-test.db-wal");
  std::remove("notification-controller-test.db-shm");
}
