#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/enums.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <shared/services/sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace
{
constexpr const char* kIdentityDb = "audit-sync-read-test-identity.db";
constexpr const char* kLegacyDb = "audit-sync-read-test-legacy.db";

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
  DbService::setReadOnlyClient(drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=file:") + kLegacyDb + "?mode=ro", 1));

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

  drogon::app().quit();
  runner.join();
  DbService::setReadOnlyClient(nullptr);
  std::remove(kLegacyDb);
  std::remove(kIdentityDb);
  std::filesystem::remove_all("/tmp/argus-audit-sync-read-test-upload");
}
