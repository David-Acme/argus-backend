#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/WebSocketConnection.h>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/transport/dtos/synchronized-dto.hxx>
#include <feature/transport/services/synchronized-service.hxx>
#include <auth/jwt-filter.hxx>
#include <sync/module-audit-event.hxx>
#include <feature/transport/infra/notification-sync-source.hxx>
#include <feature/transport/infra/productivity-sync-source.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <sync/user-audit-event.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <shared/services/room/room-manager.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <auth/user-role.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <trantor/net/EventLoop.h>
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

struct SeedAuditTablesInput
{
  const char* path;
  int64_t auditId{0};
  int64_t userAuditId{0};
};

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

void exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "exec failed";
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
  sqlite3_free(error);
}

DbHandle openFile(const std::string& path)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
    const std::string message = raw ? sqlite3_errmsg(raw) : "open failed";
    sqlite3_close_v2(raw);
    throw std::runtime_error(message);
  }
  return {raw, sqlite3_close_v2};
}

void seedAuditTables(const SeedAuditTablesInput& input)
{
  const char* path = input.path;
  const int64_t auditId = input.auditId;
  const int64_t userAuditId = input.userAuditId;

  std::remove(path);
  const DbHandle db = openFile(path);
  exec(db.get(),
       "CREATE TABLE audit_log ("
       "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
       "create_user_id INTEGER, record_id INTEGER NOT NULL, "
       "table_name TEXT NOT NULL, changes TEXT NOT NULL DEFAULT '{}', "
       "priority INTEGER NOT NULL DEFAULT 1, event_timestamp INTEGER NOT NULL, "
       "created_at INTEGER NOT NULL DEFAULT 0)");
  exec(db.get(),
       "CREATE TABLE user_audit_log ("
       "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
       "user_id INTEGER NOT NULL, record_id INTEGER NOT NULL, "
       "table_name TEXT NOT NULL, changes TEXT NOT NULL DEFAULT '{}', "
       "priority INTEGER NOT NULL DEFAULT 1, event_timestamp INTEGER NOT NULL, "
       "created_at INTEGER NOT NULL DEFAULT 0)");
  exec(db.get(), "CREATE INDEX idx_audit_log_record "
                 "ON audit_log (record_id, table_name)");
  exec(db.get(), "CREATE INDEX idx_user_audit_log_record "
                 "ON user_audit_log (record_id, table_name)");
  exec(db.get(), "INSERT INTO audit_log (id, record_id, table_name, changes, "
                 "priority, event_timestamp) VALUES (" +
                     std::to_string(auditId) + ", 1, 'camera', '{}', 1, 100)");
  exec(db.get(),
       "INSERT INTO user_audit_log (id, user_id, record_id, table_name, "
       "changes, priority, event_timestamp) VALUES (" +
           std::to_string(userAuditId) + ", 7, 1, 'camera', '{}', 1, 100)");
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

void drain(const drogon::orm::DbClientPtr& client)
{
  auto drained = std::make_shared<std::promise<void>>();
  auto done = drained->get_future();
  client->execSqlAsync(
      "SELECT 1",
      [drained](const drogon::orm::Result&) {
        trantor::EventLoop::getEventLoopOfCurrentThread()->queueInLoop(
            [drained]() { drained->set_value(); });
      },
      [drained](const std::exception_ptr& e) {
        try {
          std::rethrow_exception(e);
        }
        catch (const std::exception& ex) {
          std::fprintf(stderr, "drain statement failed: %s\n", ex.what());
        }
        drained->set_value();
      });
  if (done.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    throw std::runtime_error("the client's loop did not drain");
}

class FakeProductivitySource final : public ProductivitySyncSource
{
public:
  bool serves(ProductivitySyncTable) const override { return true; }

  std::unique_ptr<Syncable>
  sourceFor(ProductivitySyncTable table, const JwtContext& ctx) const override
  {
    lastTable = table;
    lastUser = ctx.sub;
    return std::make_unique<Pull>(ctx.sub);
  }

  mutable ProductivitySyncTable lastTable{ProductivitySyncTable::Reminder};
  mutable int64_t lastUser{0};

private:
  class Pull final : public Syncable
  {
  public:
    explicit Pull(int64_t userId) : userId_(userId) {}

    drogon::Task<std::vector<Json::Value>>
    find(const SyncFilter&) const override
    {
      std::vector<Json::Value> rows;
      Json::Value row(Json::objectValue);
      row["id"] = Json::Int64(userId_ == 42 ? 3 : 4);
      row["ownerId"] = Json::Int64(userId_);
      rows.push_back(row);
      if (userId_ == 42) {
        Json::Value shared(Json::objectValue);
        shared["id"] = Json::Int64(4);
        shared["ownerId"] = Json::Int64(7);
        rows.push_back(shared);
      }
      co_return rows;
    }

    drogon::Task<std::vector<Json::Value>>
    findDeleted(const SyncFilter&) const override
    {
      co_return std::vector<Json::Value>{};
    }

    drogon::Task<std::optional<Json::Value>>
    findLast(const SyncFilter&) const override
    {
      co_return std::nullopt;
    }

    drogon::Task<std::optional<Json::Value>>
    findLastDeleted(const SyncFilter&) const override
    {
      co_return std::nullopt;
    }

  private:
    int64_t userId_;
  };
};

class FakeNotificationSource final : public NotificationSyncSource
{
public:
  drogon::Task<std::vector<Json::Value>>
  find(const JwtContext& ctx, const SyncFilter&) const override
  {
    lastUser = ctx.sub;
    std::vector<Json::Value> rows;
    if (ctx.sub == 7) {
      Json::Value row(Json::objectValue);
      row["id"] = Json::Int64(1);
      row["userId"] = Json::Int64(7);
      rows.push_back(row);
    }
    co_return rows;
  }

  drogon::Task<std::optional<Json::Value>>
  findLast(const JwtContext&) const override
  {
    co_return std::nullopt;
  }

  mutable int64_t lastUser{0};
};

class RecordingConnection final : public drogon::WebSocketConnection
{
public:
  void send(const char* msg, uint64_t len,
            const drogon::WebSocketMessageType type) override
  {
    (void)type;
    messages.emplace_back(msg, len);
  }
  void send(std::string_view msg,
            const drogon::WebSocketMessageType type) override
  {
    (void)type;
    messages.emplace_back(msg);
  }
  void sendJson(const Json::Value& json,
                const drogon::WebSocketMessageType type) override
  {
    (void)type;
    messages.push_back(json_util::toString(json));
  }
  const trantor::InetAddress& localAddr() const override { return addr_; }
  const trantor::InetAddress& peerAddr() const override { return addr_; }
  bool connected() const override { return true; }
  bool disconnected() const override { return false; }
  void shutdown(const drogon::CloseCode, const std::string&) override {}
  void forceClose() override {}
  void setPingMessage(const std::string&,
                      const std::chrono::duration<double>&) override
  {
  }
  void disablePing() override {}

  std::vector<std::string> messages;

private:
  trantor::InetAddress addr_{"127.0.0.1", 0};
};

bool waitForMessages(const std::shared_ptr<RecordingConnection>& conn,
                     std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!conn->messages.empty())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return !conn->messages.empty();
}
}

TEST_CASE("audit sync reads resolve to the default client, not the "
          "read-only one")
{
  DbService::enableUriFilenames();

  const TempDb legacyDbFile("audit-sync-read-legacy");
  const TempDb ownDbFile("audit-sync-read-own");
  seedAuditTables(
      {.path = legacyDbFile.path().c_str(), .auditId = 1, .userAuditId = 1});
  seedAuditTables(
      {.path = ownDbFile.path().c_str(), .auditId = 2, .userAuditId = 2});

  std::filesystem::remove_all("/tmp/argus-audit-sync-read-test-upload");
  drogon::app().setUploadPath("/tmp/argus-audit-sync-read-test-upload");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, ownDbFile.path(), "default", -1});

  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  const auto legacyDb = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=file:") + legacyDbFile.path() + "?mode=ro", 1);
  DbService::setReadOnlyClient(legacyDb);

  const AuditLogRepository auditRepository;
  const UserAuditLogRepository userAuditRepository;

  AuditLogSyncFilter auditFilter;
  auditFilter.tableNames = {TableName::Camera};
  const auto auditRows = drogon::sync_wait(auditRepository.findSync(auditFilter));
  REQUIRE(auditRows.size() == 1);
  CHECK(auditRows.front()["id"].asInt64() == 2);
  const auto auditLast = drogon::sync_wait(auditRepository.findLastSync(auditFilter));
  REQUIRE(auditLast);
  CHECK((*auditLast)["id"].asInt64() == 2);

  UserAuditLogSyncFilter userAuditFilter;
  userAuditFilter.userId = 7;
  const auto userRows =
      drogon::sync_wait(userAuditRepository.findSync(userAuditFilter));
  REQUIRE(userRows.size() == 1);
  CHECK(userRows.front()["id"].asInt64() == 2);
  CHECK(userRows.front()["userId"].asInt64() == 7);
  const auto userLast =
      drogon::sync_wait(userAuditRepository.findLastSync(userAuditFilter));
  REQUIRE(userLast);
  CHECK((*userLast)["id"].asInt64() == 2);

  DbService::setReadOnlyClient(nullptr);
  const auto auditRowsAfter =
      drogon::sync_wait(auditRepository.findSync(auditFilter));
  REQUIRE(auditRowsAfter.size() == 1);
  CHECK(auditRowsAfter.front()["id"].asInt64() == 2);

  const auto conn = std::make_shared<RecordingConnection>();
  RoomManager rooms;
  AuditFanOut auditFanOut;
  drogon::app().getIOLoop(0)->runInLoop(
      [&] { rooms.join(moduleRoom(TableName::Camera), conn); });

  const Json::Value before = [&] {
    Json::Value v;
    v["id"] = Json::Int64(7);
    v["name"] = "Front door";
    return v;
  }();
  const Json::Value after = [&] {
    Json::Value v;
    v["id"] = Json::Int64(7);
    v["name"] = "Back door";
    return v;
  }();

  ModuleAuditEvent event;
  event.recordId = 7;
  event.tableName = TableName::Camera;
  event.changes = JsonDiff::createFlatDiff(before, after);
  event.createUserId = 42;
  event.eventTimestamp = 1735689600000;
  const std::string changesText =
      json_util::toString(JsonDiff::toJson(event.changes));

  drogon::app().getIOLoop(0)->runInLoop([&event, &auditFanOut] {
    drogon::async_run([&event, &auditFanOut]() -> drogon::Task<void> {
      co_await auditFanOut.handleAuditChange(event.toJson());
      co_return;
    });
  });
  REQUIRE(waitForMessages(conn, std::chrono::seconds(5)));
  drogon::app().getIOLoop(0)->runInLoop(
      [&] { rooms.leave(moduleRoom(TableName::Camera), conn); });

  REQUIRE(conn->messages.size() == 1);
  const Json::Value fanned = json_util::fromString(conn->messages.front());
  CHECK(fanned["operation"].asInt() == static_cast<int>(SyncOperation::Log));
  CHECK(fanned["option"] == "camera");
  const Json::Value info = fanned["info"];
  CHECK(info["id"].asInt64() > 0);
  CHECK(info["recordId"].asInt64() == 7);
  CHECK(info["tableName"] == "camera");
  CHECK(info["createUserId"].asInt64() == 42);
  CHECK(info["eventTimestamp"].asInt64() == 1735689600000);
  CHECK(json_util::toString(info["changes"]) == changesText);

  AuditLogSyncFilter funnelFilter;
  funnelFilter.tableNames = {TableName::Camera};
  funnelFilter.afterId = info["id"].asInt64() - 1;
  const auto funnelRows = drogon::sync_wait(auditRepository.findSync(funnelFilter));
  REQUIRE(funnelRows.size() == 1);
  CHECK(json_util::toString(funnelRows.front()["changes"]) == changesText);
  CHECK(funnelRows.front()["recordId"].asInt64() == 7);
  CHECK(funnelRows.front()["createUserId"].asInt64() == 42);
  CHECK(funnelRows.front()["eventTimestamp"].asInt64() == 1735689600000);

  const auto userConn = std::make_shared<RecordingConnection>();
  drogon::app().getIOLoop(0)->runInLoop(
      [&] { rooms.join(userRoom(42), userConn); });

  const Json::Value projectBefore = [&] {
    Json::Value v;
    v["id"] = Json::Int64(9);
    v["name"] = "Chores";
    return v;
  }();
  const Json::Value projectAfter = [&] {
    Json::Value v;
    v["id"] = Json::Int64(9);
    v["name"] = "Renovations";
    return v;
  }();

  UserAuditEvent userEvent;
  userEvent.recordId = 9;
  userEvent.tableName = TableName::Project;
  userEvent.changes = JsonDiff::createFlatDiff(projectBefore, projectAfter);
  userEvent.users = {42};
  userEvent.eventTimestamp = 1735689600000;
  const std::string userChangesText =
      json_util::toString(JsonDiff::toJson(userEvent.changes));

  drogon::app().getIOLoop(0)->runInLoop([&userEvent, &auditFanOut] {
    drogon::async_run([&userEvent, &auditFanOut]() -> drogon::Task<void> {
      co_await auditFanOut.handleAuditChange(userEvent.toJson());
      co_return;
    });
  });
  REQUIRE(waitForMessages(userConn, std::chrono::seconds(5)));
  drogon::app().getIOLoop(0)->runInLoop(
      [&] { rooms.leave(userRoom(42), userConn); });

  REQUIRE(userConn->messages.size() == 1);
  const Json::Value userFanned = json_util::fromString(userConn->messages.front());
  CHECK(userFanned["operation"].asInt()
        == static_cast<int>(SyncOperation::Log));
  CHECK(userFanned["option"] == "user_audit_log");
  const Json::Value userInfo = userFanned["info"];
  CHECK(userInfo["id"].asInt64() > 0);
  CHECK(userInfo["userId"].asInt64() == 42);
  CHECK(userInfo["recordId"].asInt64() == 9);
  CHECK(userInfo["tableName"] == "project");
  CHECK(userInfo["eventTimestamp"].asInt64() == 1735689600000);
  CHECK(json_util::toString(userInfo["changes"]) == userChangesText);

  UserAuditLogSyncFilter funnelUserFilter;
  funnelUserFilter.userId = 42;
  funnelUserFilter.afterId = userInfo["id"].asInt64() - 1;
  const auto funnelUserRows =
      drogon::sync_wait(userAuditRepository.findSync(funnelUserFilter));
  REQUIRE(funnelUserRows.size() == 1);
  CHECK(json_util::toString(funnelUserRows.front()["changes"])
        == userChangesText);
  CHECK(funnelUserRows.front()["recordId"].asInt64() == 9);
  CHECK(funnelUserRows.front()["tableName"] == "project");
  CHECK(funnelUserRows.front()["eventTimestamp"].asInt64() == 1735689600000);

  const Json::Value plainEvent = [&] {
    Json::Value v;
    v["operation"] = static_cast<int>(SyncOperation::Add);
    v["option"] = "project";
    Json::Value row;
    row["id"] = Json::Int64(9);
    v["info"] = row;
    Json::Value users(Json::arrayValue);
    users.append(42);
    v["users"] = users;
    return v;
  }();
  const auto plainConn = std::make_shared<RecordingConnection>();
  drogon::app().getIOLoop(0)->runInLoop(
      [&] { rooms.join(userRoom(42), plainConn); });
  const auto plainFanout = sync_fan_out::parseEvent(plainEvent);
  REQUIRE(plainFanout);
  drogon::app().getIOLoop(0)->runInLoop(
      [fanout = *plainFanout] { sync_fan_out::dispatchEvent(fanout); });
  REQUIRE(waitForMessages(plainConn, std::chrono::seconds(5)));
  drogon::app().getIOLoop(0)->runInLoop(
      [&] { rooms.leave(userRoom(42), plainConn); });
  REQUIRE(plainConn->messages.size() == 1);
  const Json::Value plainFanned =
      json_util::fromString(plainConn->messages.front());
  CHECK(plainFanned["operation"].asInt()
        == static_cast<int>(SyncOperation::Add));
  CHECK(plainFanned["option"] == "project");
  CHECK(plainFanned["info"]["id"].asInt64() == 9);

  SynchronizedService synchronizedService;
  FakeProductivitySource productivitySource;
  FakeNotificationSource notificationSource;
  synchronizedService.setProductivitySource(&productivitySource);
  synchronizedService.setNotificationSource(&notificationSource);

  const JwtContext ownerCtx{.sub = 42, .name = "Owner", .role = UserRole::Owner, .isActive = true, .deviceHash = {}, .sessionId = {}};
  const JwtContext residentCtx{.sub = 7, .name = "Resident", .role = UserRole::Resident, .isActive = true, .deviceHash = {}, .sessionId = {}};

  Json::Value projectSyncBody;
  Json::Value projectBody;
  projectBody["requiredCreate"] = true;
  projectSyncBody["project"] = projectBody;
  const auto ownerSync = drogon::sync_wait(
      synchronizedService.sync(SynchronizedDto::fromJson(projectSyncBody),
                               ownerCtx));
  REQUIRE(ownerSync["info"]["project"]["created"].size() == 2);
  CHECK(ownerSync["info"]["project"]["created"][0]["id"].asInt64() == 3);
  CHECK(ownerSync["info"]["project"]["created"][1]["id"].asInt64() == 4);
  CHECK(productivitySource.lastTable == ProductivitySyncTable::Project);
  CHECK(productivitySource.lastUser == 42);

  const auto residentSync = drogon::sync_wait(
      synchronizedService.sync(SynchronizedDto::fromJson(projectSyncBody),
                               residentCtx));
  REQUIRE(residentSync["info"]["project"]["created"].size() == 1);
  CHECK(residentSync["info"]["project"]["created"][0]["id"].asInt64() == 4);
  CHECK(productivitySource.lastUser == 7);

  Json::Value notificationSyncBody;
  Json::Value notificationBody;
  notificationBody["requiredCreate"] = true;
  notificationSyncBody["notification"] = notificationBody;
  const auto otherSync = drogon::sync_wait(
      synchronizedService.sync(
          SynchronizedDto::fromJson(notificationSyncBody), ownerCtx));
  CHECK(otherSync["info"]["notification"]["created"].size() == 0);
  CHECK(notificationSource.lastUser == 42);
  const auto ownSync = drogon::sync_wait(
      synchronizedService.sync(
          SynchronizedDto::fromJson(notificationSyncBody), residentCtx));
  REQUIRE(ownSync["info"]["notification"]["created"].size() == 1);
  CHECK(ownSync["info"]["notification"]["created"][0]["id"].asInt64() == 1);
  CHECK(notificationSource.lastUser == 7);

  SynchronizedService bare;
  bool unavailable = false;
  try {
    drogon::sync_wait(
        bare.sync(SynchronizedDto::fromJson(projectSyncBody), ownerCtx));
  }
  catch (const ResponseException& error) {
    unavailable = error.statusCode() == 503;
  }
  CHECK(unavailable);

  DbService::client()->execSqlSync(
      "CREATE TABLE audit_compaction_state (table_name TEXT NOT NULL PRIMARY "
      "KEY, compacted_through_id INTEGER NOT NULL DEFAULT 0)");
  DbService::client()->execSqlSync(
      "INSERT INTO audit_compaction_state (table_name, compacted_through_id) "
      "VALUES ('audit_log', 5), ('user_audit_log', 3)");

  const auto logBody = [](int64_t afterId) {
    Json::Value body;
    body["afterId"] = static_cast<Json::Int64>(afterId);
    return SynchronizedLogDto::fromJson(body);
  };

  const auto freshBaseline =
      drogon::sync_wait(synchronizedService.syncAuditLog(logBody(0), ownerCtx));
  CHECK(freshBaseline["info"]["info"].isArray());

  const auto currentCursor =
      drogon::sync_wait(synchronizedService.syncAuditLog(logBody(5), ownerCtx));
  CHECK(currentCursor["info"]["info"].isArray());

  bool moduleRefused = false;
  try {
    drogon::sync_wait(synchronizedService.syncAuditLog(logBody(4), ownerCtx));
  }
  catch (const ResponseException& error) {
    moduleRefused = error.statusCode() == 409 && error.errorCode() == "CONFLICT";
  }
  CHECK(moduleRefused);

  const auto userBaseline = drogon::sync_wait(
      synchronizedService.syncUserAuditLog(logBody(0), ownerCtx));
  CHECK(userBaseline["info"]["info"].isArray());

  const auto userCurrent = drogon::sync_wait(
      synchronizedService.syncUserAuditLog(logBody(3), ownerCtx));
  CHECK(userCurrent["info"]["info"].isArray());

  bool userRefused = false;
  try {
    drogon::sync_wait(
        synchronizedService.syncUserAuditLog(logBody(2), ownerCtx));
  }
  catch (const ResponseException& error) {
    userRefused = error.statusCode() == 409 && error.errorCode() == "CONFLICT";
  }
  CHECK(userRefused);

  drain(legacyDb);
  DbService::setReadOnlyClient(nullptr);
  std::filesystem::remove_all("/tmp/argus-audit-sync-read-test-upload");
}
