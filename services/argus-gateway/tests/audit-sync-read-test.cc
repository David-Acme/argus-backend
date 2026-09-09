#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/WebSocketConnection.h>
#include <drogon/drogon.h>
#include <feature/socket/sync/dtos/synchronized-dto.hxx>
#include <feature/socket/sync/services/synchronized-service.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/contracts/camera-audit-event.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/enums.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <shared/services/room/room-manager.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-diff/json-diff.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/contracts/user-audit-event.hxx>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/repositories/project/project-repository.hxx>
#include <sync/camera-fan-out.hxx>
#include <sync/user-change-fan-out.hxx>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kIdentityDb = "audit-sync-read-test-identity.db";
constexpr const char* kLegacyDb = "audit-sync-read-test-legacy.db";
constexpr const char* kCameraDb = "audit-sync-read-test-camera.db";
constexpr const char* kProductivityDb =
    "audit-sync-read-test-productivity.db";
constexpr const char* kNotificationDb =
    "audit-sync-read-test-notification.db";

void seedAuditTables(const char* path, int64_t auditId, int64_t userAuditId)
{
  std::remove(path);
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
                                              1);
  client->execSqlSync(
      "CREATE TABLE audit_log ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "create_user_id INTEGER, record_id INTEGER NOT NULL, "
      "table_name TEXT NOT NULL, changes TEXT NOT NULL DEFAULT '{}', "
      "priority INTEGER NOT NULL DEFAULT 1, event_timestamp INTEGER NOT NULL, "
      "created_at INTEGER NOT NULL DEFAULT 0)");
  client->execSqlSync(
      "CREATE TABLE user_audit_log ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "user_id INTEGER NOT NULL, record_id INTEGER NOT NULL, "
      "table_name TEXT NOT NULL, changes TEXT NOT NULL DEFAULT '{}', "
      "priority INTEGER NOT NULL DEFAULT 1, event_timestamp INTEGER NOT NULL, "
      "created_at INTEGER NOT NULL DEFAULT 0)");
  client->execSqlSync("INSERT INTO audit_log (id, record_id, table_name, "
                      "changes, priority, event_timestamp) "
                      "VALUES (?, 1, 'camera', '{}', 1, 100)",
                      auditId);
  client->execSqlSync("INSERT INTO user_audit_log (id, user_id, record_id, "
                      "table_name, changes, priority, event_timestamp) "
                      "VALUES (?, 7, 1, 'camera', '{}', 1, 100)",
                      userAuditId);
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

// Camera table (camera.db shape) for the named-camera-client resolution test.
void createCameraTable(const char* path, int64_t id, const char* name)
{
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
                                              1);
  client->execSqlSync(
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
  client->execSqlSync(
      "INSERT INTO camera (id, name, ip) VALUES (?, ?, '127.0.0.1')", id, name);
}

void seedCameraTable(const char* path, int64_t id, const char* name)
{
  std::remove(path);
  createCameraTable(path, id, name);
}

// Project table (productivity.db shape) for the named-productivity-client test.
void createProjectTable(const char* path, int64_t id, const char* name)
{
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
                                              1);
  client->execSqlSync(
      "CREATE TABLE project ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "owner_id INTEGER NOT NULL, name TEXT NOT NULL, "
      "description TEXT NOT NULL DEFAULT '', "
      "status TEXT NOT NULL DEFAULT 'active', "
      "color TEXT NOT NULL DEFAULT '', starts_at INTEGER, "
      "target_at INTEGER, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  client->execSqlSync("INSERT INTO project (id, owner_id, name) "
                      "VALUES (?, 7, ?)",
                      id, name);
}

void seedProjectTable(const char* path, int64_t id, const char* name)
{
  std::remove(path);
  createProjectTable(path, id, name);
}

void insertProjectRow(const char* path, int64_t id, int64_t ownerId,
                      const char* name)
{
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
                                              1);
  client->execSqlSync("INSERT INTO project (id, owner_id, name) VALUES (?, ?, ?)",
                      id, ownerId, name);
}

// The project sync query probes project_member for shared access.
void createProjectMemberTable(const char* path)
{
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
                                              1);
  client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS project_member ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "project_id INTEGER NOT NULL REFERENCES project(id) ON DELETE CASCADE, "
      "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "access TEXT NOT NULL DEFAULT 'view' "
      "CHECK (access IN ('view', 'edit')), "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
}

// Notification table (notification.db shape) for substrate resolution.
void createNotificationTable(const char* path)
{
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
                                              1);
  client->execSqlSync(
      "CREATE TABLE notification ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "user_id INTEGER NOT NULL, "
      "type TEXT NOT NULL DEFAULT 'system', "
      "title TEXT NOT NULL DEFAULT '', "
      "body TEXT NOT NULL DEFAULT '', "
      "data TEXT NOT NULL DEFAULT '{}', "
      "is_read INTEGER NOT NULL DEFAULT 0 CHECK (is_read IN (0, 1)), "
      "read_at INTEGER, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
}

void seedNotificationRow(const char* path, int64_t id, const char* title)
{
  createNotificationTable(path);
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
                                              1);
  client->execSqlSync(
      "INSERT INTO notification (id, user_id, title) VALUES (?, 7, ?)",
      id, title);
}


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

  seedAuditTables(kLegacyDb, 1, 1);
  seedAuditTables(kIdentityDb, 2, 2);

  std::filesystem::remove_all("/tmp/argus-audit-sync-read-test-upload");
  drogon::app().setUploadPath("/tmp/argus-audit-sync-read-test-upload");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kIdentityDb, "default", -1});

  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  // The client object must outlive the in-flight callbacks on its own loop.
  const auto legacyDb = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=file:") + kLegacyDb + "?mode=ro", 1);
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
  createCameraTable(kIdentityDb, 1, "default row");
  seedCameraTable(kCameraDb, 2, "camera-db row");

  const auto cameraDb = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=") + kCameraDb, 1);
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

  // ── Phase: named productivity client resolution (Ruling AQ) ──────────────
  createProjectTable(kIdentityDb, 1, "default row");
  seedProjectTable(kProductivityDb, 2, "productivity-db row");

  const auto productivityDb = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=file:") + kProductivityDb + "?mode=ro", 1);
  DbService::setProductivityClient(productivityDb);

  const ProjectRepository projectRepository;
  const auto fromProductivity = drogon::sync_wait(projectRepository.findById(2));
  REQUIRE(fromProductivity);
  CHECK(fromProductivity->name == "productivity-db row");

  const auto notSharedProject =
      drogon::sync_wait(projectRepository.findById(1));
  CHECK_FALSE(notSharedProject);

  DbService::setProductivityClient(nullptr);
  const auto projectFallback =
      drogon::sync_wait(projectRepository.findById(1));
  REQUIRE(projectFallback);
  CHECK(projectFallback->name == "default row");

  // ── Phase: notification substrate (Ruling AR) ────────────────────────────
  seedNotificationRow(kIdentityDb, 1, "default row");
  seedNotificationRow(kNotificationDb, 2, "notification-db row");

  const auto notificationDb = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=") + kNotificationDb, 1);
  DbService::setNotificationClient(notificationDb);

  const NotificationRepository notificationRepository;
  NotificationSyncFilter notificationFilter;
  notificationFilter.userId = 7;
  const auto notificationRows =
      drogon::sync_wait(notificationRepository.findSync(notificationFilter));
  REQUIRE(notificationRows.size() == 1);
  CHECK(notificationRows.front()["id"].asInt64() == 2);
  CHECK(notificationRows.front()["title"] == "notification-db row");

  NotificationCreateInput createInput;
  createInput.userId = 7;
  createInput.type = "system";
  createInput.title = "gateway created";
  createInput.body = "body";
  createInput.data = Json::Value(Json::objectValue);
  const auto created =
      drogon::sync_wait(notificationRepository.create(createInput));
  CHECK(created.id > 2);

  const auto readChanges =
      drogon::sync_wait(notificationRepository.markAsRead(7, {2}));
  REQUIRE(readChanges.size() == 1);
  CHECK(readChanges.front().before.id == 2);
  CHECK(readChanges.front().before.isRead == 0);
  CHECK(readChanges.front().after.id == 2);
  CHECK(readChanges.front().after.isRead == 1);

  DbService::setNotificationClient(nullptr);
  const auto notificationFallback =
      drogon::sync_wait(notificationRepository.findSync(notificationFilter));
  REQUIRE(notificationFallback.size() == 1);
  CHECK(notificationFallback.front()["id"].asInt64() == 1);

  // ── Phase: personal-table sync scoping (Ruling AQ) ───────────────────────
  createProjectMemberTable(kIdentityDb);
  insertProjectRow(kIdentityDb, 3, 42, "owned by 42");
  insertProjectRow(kIdentityDb, 4, 7, "owned by 7");
  auto memberClient = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=") + kIdentityDb, 1);
  memberClient->execSqlSync(
      "INSERT INTO project_member (id, project_id, user_id, access) "
      "VALUES (1, 4, 42, 'edit')");
  memberClient.reset();

  const SynchronizedService synchronizedService;
  const JwtContext ownerCtx{42, "Owner", UserRole::Owner, true, {}};
  const JwtContext residentCtx{7, "Resident", UserRole::Resident, true, {}};

  Json::Value projectSyncBody;
  Json::Value projectBody;
  projectBody["requiredCreate"] = true;
  projectSyncBody["project"] = projectBody;
  const auto ownerSync = drogon::sync_wait(
      synchronizedService.sync(SynchronizedDto::fromJson(projectSyncBody),
                               ownerCtx));
  // Own project plus the one shared with them as a member, never others'.
  REQUIRE(ownerSync["info"]["project"]["created"].size() == 2);
  CHECK(ownerSync["info"]["project"]["created"][0]["id"].asInt64() == 3);
  CHECK(ownerSync["info"]["project"]["created"][1]["id"].asInt64() == 4);
  const auto residentSync = drogon::sync_wait(
      synchronizedService.sync(SynchronizedDto::fromJson(projectSyncBody),
                               residentCtx));
  REQUIRE(residentSync["info"]["project"]["created"].size() == 2);
  CHECK(residentSync["info"]["project"]["created"][0]["id"].asInt64() == 1);
  CHECK(residentSync["info"]["project"]["created"][1]["id"].asInt64() == 4);

  // The notification page keeps its user scoping the same way.
  Json::Value notificationSyncBody;
  Json::Value notificationBody;
  notificationBody["requiredCreate"] = true;
  notificationSyncBody["notification"] = notificationBody;
  const auto otherSync = drogon::sync_wait(
      synchronizedService.sync(
          SynchronizedDto::fromJson(notificationSyncBody), ownerCtx));
  CHECK(otherSync["info"]["notification"]["created"].size() == 0);
  const auto ownSync = drogon::sync_wait(
      synchronizedService.sync(
          SynchronizedDto::fromJson(notificationSyncBody), residentCtx));
  REQUIRE(ownSync["info"]["notification"]["created"].size() == 1);
  CHECK(ownSync["info"]["notification"]["created"][0]["id"].asInt64() == 1);

  drogon::app().quit();
  runner.join();
  DbService::setReadOnlyClient(nullptr);
  DbService::setCameraClient(nullptr);
  DbService::setProductivityClient(nullptr);
  DbService::setNotificationClient(nullptr);
  std::remove(kLegacyDb);
  std::remove(kIdentityDb);
  std::remove(kCameraDb);
  std::remove(kProductivityDb);
  std::remove(kNotificationDb);
  std::remove("audit-sync-read-test-notification.db-wal");
  std::remove("audit-sync-read-test-notification.db-shm");
  std::filesystem::remove_all("/tmp/argus-audit-sync-read-test-upload");
}
