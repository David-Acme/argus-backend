#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/WebSocketConnection.h>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/socket/sync/dtos/synchronized-dto.hxx>
#include <feature/socket/sync/services/synchronized-service.hxx>
#include <auth/jwt-filter.hxx>
#include <shared/contracts/camera-audit-event.hxx>
#include <shared/contracts/notification-sync-source.hxx>
#include <shared/contracts/productivity-sync-source.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <shared/contracts/user-audit-event.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <shared/services/room/room-manager.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>
#include <sync/camera-fan-out.hxx>
#include <sync/user-change-fan-out.hxx>
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

// The drogon calls these replace all threw on failure, so a throwing helper
// keeps the suite's assertion count where it was.
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

// Seeded through the sqlite3 C API, the way this unit's sibling suites seed
// theirs: a throwaway drogon client keeps a loop thread of its own, and a
// connection whose queued statement lambda still holds the last reference is
// destroyed on that thread, where ~EventLoopThread then joins the thread it is
// running on (EDEADLK -> SIGABRT, no assertion reported). The default rollback
// journal is kept on purpose: a read-only client opens one of these files
// afterwards, and a WAL database needs write access for its -shm.
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

// Runs the app and stops it however the case body leaves. A joinable
// std::thread destroyed by unwinding calls std::terminate, which reports an
// ordinary statement failure as a SIGABRT with no assertion behind it.
class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    // Drogon reports the app running before its main loop is looping, and a
    // loop that has not begun cannot be stopped: trantor's loop() clears the
    // quit flag again as it starts. Waiting for it to loop is what makes the
    // quit below take effect — detaching in that window left the app's thread
    // running past the end of the process, measured as SIGSEGV inside
    // EventLoop::loop() in 3 of 20 runs of a forced constructor throw.
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    // A boot that never reached the loop at all is left to the process: it
    // cannot be asked to stop, and joining it would block for ever.
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::thread runner_;
};

// One async statement whose callback queues a sentinel on the connection's own
// loop. Callbacks run on that loop, and trantor destroys each queued functor as
// it dequeues the next, so the reference the statement lambda held is gone
// before the sentinel runs: after this returns no thread but this one holds the
// connection, and the client's destructor joins an idle loop thread from
// outside instead of its own. The callback must not capture the client: a
// reference released on that loop re-opens the window. No assertion here by
// design (a timeout throws): the suites' counts must not move.
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

// Camera table (camera.db shape) for the named-camera-client resolution test.
struct CreateCameraTableInput
{
  const char* path;
  int64_t id{0};
  const char* name;
};

void createCameraTable(const CreateCameraTableInput& input)
{
  const char* path = input.path;
  const int64_t id = input.id;
  const char* name = input.name;

  const DbHandle db = openFile(path);
  // The app's default client holds this file while the identity database is
  // seeded, so take the tree's bootstrap and wait out a lock instead of
  // answering SQLITE_BUSY at once.
  exec(db.get(), "PRAGMA busy_timeout = 5000");
  exec(db.get(), "PRAGMA journal_mode = WAL");
  exec(db.get(),
       "CREATE TABLE camera ("
       "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
       "name TEXT NOT NULL, manufacturer TEXT NOT NULL DEFAULT '', "
       "model TEXT NOT NULL DEFAULT '', ip TEXT NOT NULL, "
       "port INTEGER NOT NULL DEFAULT 554, "
       "username TEXT NOT NULL DEFAULT 'admin', "
       "password TEXT NOT NULL DEFAULT '', "
       "cloud_username TEXT NOT NULL DEFAULT '', "
       "cloud_password TEXT NOT NULL DEFAULT '', "
       "driver TEXT NOT NULL DEFAULT 'tapo', "
       "icon TEXT NOT NULL DEFAULT 'video', "
       "record_mode TEXT NOT NULL DEFAULT 'events', "
       "retention_days INTEGER, capabilities TEXT NOT NULL DEFAULT '[]', "
       "config TEXT NOT NULL DEFAULT '{}', "
       "is_enabled INTEGER NOT NULL DEFAULT 1, "
       "is_online INTEGER NOT NULL DEFAULT 0, "
       "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
       "updated_at INTEGER, deleted_at INTEGER)");
  exec(db.get(), "INSERT INTO camera (id, name, ip) VALUES (" +
                     std::to_string(id) + ", '" + name + "', '127.0.0.1')");
}

struct SeedCameraTableInput
{
  const char* path;
  int64_t id{0};
  const char* name;
};

void seedCameraTable(const SeedCameraTableInput& input)
{
  const char* path = input.path;
  const int64_t id = input.id;
  const char* name = input.name;

  std::remove(path);
  createCameraTable({.path = path, .id = id, .name = name});
}

// Routes productivity/notification tables to a source, as production does.
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
} // namespace

TEST_CASE("audit sync reads resolve to the default identity client on the "
          "gateway")
{
  // The gateway calls this first in main; URI filenames must precede client init.
  DbService::enableUriFilenames();

  const TempDb legacyDbFile("audit-sync-read-legacy");
  const TempDb identityDbFile("audit-sync-read-identity");
  const TempDb cameraDbFile("audit-sync-read-camera");
  seedAuditTables(
      {.path = legacyDbFile.path().c_str(), .auditId = 1, .userAuditId = 1});
  seedAuditTables(
      {.path = identityDbFile.path().c_str(), .auditId = 2, .userAuditId = 2});

  std::filesystem::remove_all("/tmp/argus-audit-sync-read-test-upload");
  drogon::app().setUploadPath("/tmp/argus-audit-sync-read-test-upload");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, identityDbFile.path(), "default", -1});

  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  // The client object must outlive the in-flight callbacks on its own loop.
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

  // Audit repositories never resolve readOnlyClient(); rows come from default.
  DbService::setReadOnlyClient(nullptr);
  const auto auditRowsAfter =
      drogon::sync_wait(auditRepository.findSync(auditFilter));
  REQUIRE(auditRowsAfter.size() == 1);
  CHECK(auditRowsAfter.front()["id"].asInt64() == 2);

  // ── Phase: camera audit funnel (Ruling Y) ────────────────────────────────
  const auto conn = std::make_shared<RecordingConnection>();
  RoomManager rooms;
  // Room state is thread-local per IO loop; joins ride the dispatching loop.
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

  CameraAuditEvent event;
  event.recordId = 7;
  event.tableName = TableName::Camera;
  event.changes = JsonDiff::createFlatDiff(before, after);
  event.createUserId = 42;
  event.eventTimestamp = 1735689600000;
  const std::string changesText =
      json_util::toString(JsonDiff::toJson(event.changes));

  drogon::app().getIOLoop(0)->runInLoop(
      [&event] { camera_fan_out::handleCameraChange(event.toJson()); });
  REQUIRE(waitForMessages(conn, std::chrono::seconds(5)));
  drogon::app().getIOLoop(0)->runInLoop(
      [&] { rooms.leave(moduleRoom(TableName::Camera), conn); });

  REQUIRE(conn->messages.size() == 1);
  const Json::Value fanned = json_util::fromString(conn->messages.front());
  // The Log operation carries the DB-assigned id: insert happened first.
  CHECK(fanned["operation"].asInt() == static_cast<int>(SyncOperation::Log));
  CHECK(fanned["option"] == "camera");
  const Json::Value info = fanned["info"];
  CHECK(info["id"].asInt64() > 0);
  CHECK(info["recordId"].asInt64() == 7);
  CHECK(info["tableName"] == "camera");
  CHECK(info["createUserId"].asInt64() == 42);
  CHECK(info["eventTimestamp"].asInt64() == 1735689600000);
  CHECK(json_util::toString(info["changes"]) == changesText);

  // The audit row itself is byte-identical to what the camera produced.
  AuditLogSyncFilter funnelFilter;
  funnelFilter.tableNames = {TableName::Camera};
  funnelFilter.afterId = info["id"].asInt64() - 1;
  const auto funnelRows = drogon::sync_wait(auditRepository.findSync(funnelFilter));
  REQUIRE(funnelRows.size() == 1);
  CHECK(json_util::toString(funnelRows.front()["changes"]) == changesText);
  CHECK(funnelRows.front()["recordId"].asInt64() == 7);
  CHECK(funnelRows.front()["createUserId"].asInt64() == 42);
  CHECK(funnelRows.front()["eventTimestamp"].asInt64() == 1735689600000);

  // ── Phase: named camera client resolution (Ruling Z) ─────────────────────
  createCameraTable(
      {.path = identityDbFile.path().c_str(), .id = 1, .name = "default row"});
  seedCameraTable(
      {.path = cameraDbFile.path().c_str(), .id = 2, .name = "camera-db row"});

  const auto cameraDb = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=") + cameraDbFile.path(), 1);
  DbService::setCameraClient(cameraDb);

  const CameraRepository cameraRepository;
  const auto fromCamera = drogon::sync_wait(cameraRepository.findById(2));
  REQUIRE(fromCamera);
  CHECK(fromCamera->name == "camera-db row");

  const auto notShared = drogon::sync_wait(cameraRepository.findById(1));
  CHECK_FALSE(notShared);

  DbService::setCameraClient(nullptr);
  const auto fallback = drogon::sync_wait(cameraRepository.findById(1));
  REQUIRE(fallback);
  CHECK(fallback->name == "default row");

  const auto cameraGone = drogon::sync_wait(cameraRepository.findById(2));
  CHECK_FALSE(cameraGone);

  // ── Phase: productivity audit funnel (Ruling AQ) ─────────────────────────
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

  drogon::app().getIOLoop(0)->runInLoop(
      [&userEvent] { user_change_fan_out::handleUserChange(userEvent.toJson()); });
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

  // Byte-identical to what the producer sent, with the gateway-assigned id.
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

  // A plain user-scoped change event fans out as-is, without an insert.
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
  drogon::app().getIOLoop(0)->runInLoop([&plainEvent] {
    user_change_fan_out::handleUserChange(plainEvent);
  });
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

  // ── Phase: cross-domain sync routes through installed sources (rule 27) ─
  SynchronizedService synchronizedService;
  FakeProductivitySource productivitySource;
  FakeNotificationSource notificationSource;
  synchronizedService.setProductivitySource(&productivitySource);
  synchronizedService.setNotificationSource(&notificationSource);

  const JwtContext ownerCtx{42, "Owner", UserRole::Owner, true, {}};
  const JwtContext residentCtx{7, "Resident", UserRole::Resident, true, {}};

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

  // Without a source the gateway refuses instead of falling back to a DB.
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

  // Both handover clients die with this scope, so empty their loops first: the
  // release that reaches zero must not land on a connection's own loop thread.
  drain(legacyDb);
  drain(cameraDb);
  DbService::setReadOnlyClient(nullptr);
  DbService::setCameraClient(nullptr);
  std::filesystem::remove_all("/tmp/argus-audit-sync-read-test-upload");
}
