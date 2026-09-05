#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/WebSocketConnection.h>
#include <drogon/drogon.h>
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
#include <sync/camera-fan-out.hxx>

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

// Camera table (camera.db shape, camera-schema.sql source of truth) for the
// named-camera-client resolution test.
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
  // Gateway wiring: identity.db as the default client, legacy argus.db
  // read-only.
  seedAuditTables(kLegacyDb, 1, 1);
  seedAuditTables(kIdentityDb, 2, 2);

  std::filesystem::remove_all("/tmp/argus-audit-sync-read-test-upload");
  drogon::app().setUploadPath("/tmp/argus-audit-sync-read-test-upload");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kIdentityDb, "default", -1});

  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  DbService::enableUriFilenames();
  // The client object must outlive the in-flight callbacks on its own loop
  // thread: only the resolution is reset mid-test, the release happens after
  // the app stopped.
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

  // With or without the read-only client the rows come from the default
  // client, the resolution the legacy host always got through the fallback.
  DbService::setReadOnlyClient(nullptr);
  const auto auditRowsAfter =
      drogon::sync_wait(auditRepository.findSync(auditFilter));
  REQUIRE(auditRowsAfter.size() == 1);
  CHECK(auditRowsAfter.front()["id"].asInt64() == 2);

  // ── Phase: camera audit funnel (Ruling Y) ────────────────────────────────
  // The gateway inserts the camera-produced diff verbatim into its audit
  // substrate and only then fans the DB-assigned row out to /sync, so online
  // replay and the offline audit cursor observe the same order.
  const auto conn = std::make_shared<RecordingConnection>();
  RoomManager rooms;
  // Room state is thread-local per IO loop, so the member joins and the
  // fan-out both go through the loop that dispatches the change.
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
  // Camera-domain reads resolve to camera.db when the host installs the named
  // client and fall back to the default client when it does not (the
  // pre-cutover behavior).
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

  drogon::app().quit();
  runner.join();
  DbService::setReadOnlyClient(nullptr);
  DbService::setCameraClient(nullptr);
  std::remove(kLegacyDb);
  std::remove(kIdentityDb);
  std::remove(kCameraDb);
  std::filesystem::remove_all("/tmp/argus-audit-sync-read-test-upload");
}
