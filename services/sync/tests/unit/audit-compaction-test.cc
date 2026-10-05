#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/fanout/services/audit-log-service.hxx>
#include <feature/fanout/services/user-audit-log-service.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <sqlite/db-service.hxx>
#include <sync/audit-retention.hxx>
#include <sync/sync-limits.hxx>
#include <text/json-util.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

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

  [[nodiscard]] const std::string& path() const { return path_; }

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

inline constexpr int64_t kOldMs = 1'000'000'000'000;

struct AuditRowSeed
{
  int64_t id{0};
  int64_t recordId{0};
  const char* tableName{"camera"};
  const char* changes{"{}"};
  int priority{1};
  int64_t eventTimestamp{kOldMs};
};

void seedAuditRow(const AuditRowSeed& row)
{
  DbService::client()->execSqlSync(
      "INSERT INTO audit_log (id, record_id, table_name, changes, priority, "
      "event_timestamp) VALUES (" +
      std::to_string(row.id) + ", " + std::to_string(row.recordId) + ", '" +
      row.tableName + "', '" + row.changes + "', " +
      std::to_string(row.priority) + ", " +
      std::to_string(row.eventTimestamp) + ")");
}

struct UserAuditRowSeed
{
  int64_t id{0};
  int64_t userId{0};
  int64_t recordId{0};
  const char* tableName{"camera"};
  const char* changes{"{}"};
  int priority{1};
  int64_t eventTimestamp{kOldMs};
};

void seedUserAuditRow(const UserAuditRowSeed& row)
{
  DbService::client()->execSqlSync(
      "INSERT INTO user_audit_log (id, user_id, record_id, table_name, "
      "changes, priority, event_timestamp) VALUES (" +
      std::to_string(row.id) + ", " + std::to_string(row.userId) + ", " +
      std::to_string(row.recordId) + ", '" + row.tableName + "', '" +
      row.changes + "', " + std::to_string(row.priority) + ", " +
      std::to_string(row.eventTimestamp) + ")");
}

Json::Value storedAuditChanges(int64_t id)
{
  return json_util::fromString(
      scalar("SELECT changes FROM audit_log WHERE id = " + std::to_string(id)));
}

Json::Value storedUserAuditChanges(int64_t id)
{
  return json_util::fromString(scalar(
      "SELECT changes FROM user_audit_log WHERE id = " + std::to_string(id)));
}

inline constexpr std::string_view kBacklogOlderChanges{
    R"({"p":{"previous":"a","current":"b"}})"};
inline constexpr std::string_view kBacklogNewerChanges{
    R"({"p":{"previous":"b","current":"c"}})"};
inline constexpr int64_t kBacklogFirstId{100};

std::string auditBacklogInsert(const int64_t rows)
{
  std::string sql =
      "INSERT INTO audit_log (id, record_id, table_name, changes, priority, "
      "event_timestamp) VALUES ";
  for (int64_t i = 0; i < rows; ++i) {
    if (i > 0)
      sql += ", ";
    sql += "(" + std::to_string(kBacklogFirstId + i) + ", " +
           std::to_string(1000 + i / 2) + ", 'camera', '" +
           std::string(i % 2 == 0 ? kBacklogOlderChanges
                                  : kBacklogNewerChanges) +
           "', 1, " + std::to_string(kOldMs) + ")";
  }
  return sql;
}
}

TEST_CASE("retention compacts each audit table into its own newest rows")
{
  const TempDb db("audit-compaction-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                 .filename = db.path(),
                                 .name = "default",
                                 .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  REQUIRE(DbService::runScriptFile(ARGUS_SYNC_SCHEMA_PATH));

  const AuditLogService auditService;
  const UserAuditLogService userAuditService;
  const AuditLogRepository auditRepository;
  const UserAuditLogRepository userAuditRepository;

  const int64_t nowMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t cutoffMs =
      nowMs - static_cast<int64_t>(audit_retention::kDefaultDays) * 86'400'000;

  CHECK(scalar("SELECT COUNT(*) FROM audit_log") == "0");
  CHECK(drogon::sync_wait(auditRepository.findCompactionFrontier()) == 0);
  CHECK(drogon::sync_wait(userAuditRepository.findCompactionFrontier()) == 0);
  CHECK(drogon::sync_wait(auditService.compact({.cutoffMs = cutoffMs, .afterId = 0})).removed == 0);
  CHECK(drogon::sync_wait(userAuditService.compact({.cutoffMs = cutoffMs, .afterId = 0})).removed == 0);

  seedAuditRow({.id = 1,
                .recordId = 1,
                .changes = R"({"name":{"previous":"Front","current":"Front door"}})",
                .priority = 1,
                .eventTimestamp = kOldMs});
  seedAuditRow({.id = 2,
                .recordId = 1,
                .changes = R"({"name":{"previous":"Front door","current":"Porch"}})",
                .priority = 2,
                .eventTimestamp = kOldMs + 1000});
  seedAuditRow({.id = 3,
                .recordId = 2,
                .changes = R"({"a":{"previous":1,"current":2}})",
                .priority = 1,
                .eventTimestamp = kOldMs});
  seedAuditRow({.id = 4,
                .recordId = 2,
                .changes = R"({"b":{"previous":"x","current":"y"}})",
                .priority = 1,
                .eventTimestamp = kOldMs + 1000});
  seedAuditRow({.id = 5,
                .recordId = 2,
                .changes = R"({"c":{"previous":true,"current":false}})",
                .priority = 2,
                .eventTimestamp = kOldMs + 2000});
  seedAuditRow({.id = 6,
                .recordId = 3,
                .changes = R"({"d":{"previous":"one","current":"two"}})",
                .priority = 1,
                .eventTimestamp = kOldMs});
  seedAuditRow({.id = 7,
                .recordId = 4,
                .changes = R"({"e":{"previous":"one","current":"two"}})",
                .priority = 1,
                .eventTimestamp = nowMs - 1000});
  seedAuditRow({.id = 8,
                .recordId = 4,
                .changes = R"({"f":{"previous":"one","current":"two"}})",
                .priority = 1,
                .eventTimestamp = nowMs});
  seedAuditRow({.id = 9,
                .recordId = 5,
                .changes = R"({"g":{"previous":"one","current":"two"}})",
                .priority = 1,
                .eventTimestamp = kOldMs});
  seedAuditRow({.id = 10,
                .recordId = 5,
                .changes = R"({"h":{"previous":"one","current":"two"}})",
                .priority = 1,
                .eventTimestamp = nowMs});
  seedAuditRow({.id = 11,
                .recordId = 6,
                .changes = R"({"i":{"previous":"one","current":"two"}})",
                .priority = 1,
                .eventTimestamp = cutoffMs});
  seedAuditRow({.id = 12,
                .recordId = 6,
                .changes = R"({"j":{"previous":"one","current":"two"}})",
                .priority = 1,
                .eventTimestamp = kOldMs});
  seedAuditRow({.id = 13,
                .recordId = 7,
                .changes = R"({"k":{"previous":"one","current":"two"}})",
                .priority = 1,
                .eventTimestamp = kOldMs});
  seedAuditRow({.id = 14,
                .recordId = 7,
                .changes = R"({"l":{"previous":"one","current":"two"}})",
                .priority = 1,
                .eventTimestamp = cutoffMs});
  seedAuditRow({.id = 15,
                .recordId = 8,
                .changes = R"({"m":{"previous":"a","current":"b"}})",
                .priority = 1,
                .eventTimestamp = kOldMs});
  seedAuditRow({.id = 16,
                .recordId = 8,
                .changes = R"({"m":{"previous":"b","current":"c"}})",
                .priority = 1,
                .eventTimestamp = nowMs});
  seedAuditRow({.id = 17,
                .recordId = 8,
                .changes = R"({"n":{"previous":"x","current":"y"}})",
                .priority = 1,
                .eventTimestamp = kOldMs});

  CHECK(drogon::sync_wait(auditService.compact({.cutoffMs = cutoffMs, .afterId = 0})).removed == 3);

  CHECK(scalar("SELECT COUNT(*) FROM audit_log WHERE id IN (1, 3, 4)") == "0");
  CHECK(scalar("SELECT COUNT(*) FROM audit_log") == "14");

  const Json::Value mergedIntoTwo = storedAuditChanges(2);
  CHECK(mergedIntoTwo["name"]["previous"].asString() == "Front");
  CHECK(mergedIntoTwo["name"]["current"].asString() == "Porch");
  CHECK(scalar("SELECT priority FROM audit_log WHERE id = 2") == "2");
  CHECK(scalar("SELECT event_timestamp FROM audit_log WHERE id = 2") ==
        std::to_string(kOldMs + 1000));

  const Json::Value chained = storedAuditChanges(5);
  REQUIRE(chained.isMember("a"));
  REQUIRE(chained.isMember("b"));
  REQUIRE(chained.isMember("c"));
  CHECK(chained["a"]["previous"].asInt64() == 1);
  CHECK(chained["a"]["current"].asInt64() == 2);
  CHECK(chained["b"]["previous"].asString() == "x");
  CHECK(chained["b"]["current"].asString() == "y");
  CHECK(chained["c"]["previous"].asBool());
  CHECK_FALSE(chained["c"]["current"].asBool());
  CHECK(scalar("SELECT priority FROM audit_log WHERE id = 5") == "2");
  CHECK(scalar("SELECT event_timestamp FROM audit_log WHERE id = 5") ==
        std::to_string(kOldMs + 2000));

  for (const int64_t untouched : {6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17})
    CHECK(scalar("SELECT COUNT(*) FROM audit_log WHERE id = " +
                 std::to_string(untouched)) == "1");
  CHECK(storedAuditChanges(6)["d"]["current"].asString() == "two");
  CHECK(storedAuditChanges(12)["j"]["current"].asString() == "two");
  CHECK(storedAuditChanges(9)["g"]["current"].asString() == "two");
  CHECK(storedAuditChanges(13)["k"]["previous"].asString() == "one");
  CHECK(storedAuditChanges(14)["l"]["previous"].asString() == "one");
  CHECK(storedAuditChanges(17)["n"]["current"].asString() == "y");
  CHECK_FALSE(storedAuditChanges(17).isMember("m"));

  CHECK(drogon::sync_wait(auditRepository.findCompactionFrontier()) == 4);
  CHECK(drogon::sync_wait(userAuditRepository.findCompactionFrontier()) == 0);

  CHECK(drogon::sync_wait(auditService.compact({.cutoffMs = cutoffMs, .afterId = 0})).removed == 0);
  CHECK(scalar("SELECT COUNT(*) FROM audit_log") == "14");
  CHECK(drogon::sync_wait(auditRepository.findCompactionFrontier()) == 4);

  drogon::sync_wait(auditRepository.advanceCompactionFrontier(2));
  CHECK(drogon::sync_wait(auditRepository.findCompactionFrontier()) == 4);

  seedUserAuditRow(
      {.id = 1,
       .userId = 7,
       .recordId = 1,
       .changes = R"({"name":{"previous":"A","current":"B"}})"});
  seedUserAuditRow(
      {.id = 2,
       .userId = 8,
       .recordId = 1,
       .changes = R"({"name":{"previous":"A","current":"B"}})"});
  seedUserAuditRow(
      {.id = 3,
       .userId = 7,
       .recordId = 1,
       .changes = R"({"name":{"previous":"B","current":"C"}})"});

  CHECK(drogon::sync_wait(userAuditService.compact({.cutoffMs = cutoffMs, .afterId = 0})).removed == 1);
  CHECK(scalar("SELECT COUNT(*) FROM user_audit_log WHERE id = 1") == "0");
  CHECK(scalar("SELECT COUNT(*) FROM user_audit_log WHERE id = 2") == "1");
  CHECK(storedUserAuditChanges(3)["name"]["previous"].asString() == "A");
  CHECK(storedUserAuditChanges(3)["name"]["current"].asString() == "C");
  CHECK(storedUserAuditChanges(2)["name"]["current"].asString() == "B");
  CHECK(drogon::sync_wait(userAuditRepository.findCompactionFrontier()) == 1);
  CHECK(drogon::sync_wait(auditRepository.findCompactionFrontier()) == 4);
  CHECK(drogon::sync_wait(userAuditService.compact({.cutoffMs = cutoffMs, .afterId = 0})).removed == 0);

  const int64_t pageSize = std::stoll(SyncLimits::kMaxRows);
  const int64_t backlogRows = 2 * (pageSize + 1);
  DbService::client()->execSqlSync(auditBacklogInsert(backlogRows));

  CHECK(scalar("SELECT COUNT(*) FROM audit_log") ==
        std::to_string(14 + backlogRows));
  const auto firstPage = drogon::sync_wait(
      auditService.compact({.cutoffMs = cutoffMs, .afterId = 0}));
  CHECK(firstPage.removed == pageSize);
  CHECK(firstPage.lastOlderId == kBacklogFirstId + 2 * (pageSize - 1));
  CHECK(scalar("SELECT COUNT(*) FROM audit_log") ==
        std::to_string(14 + backlogRows - pageSize));
  CHECK(drogon::sync_wait(auditRepository.findCompactionFrontier()) ==
        kBacklogFirstId + 2 * (pageSize - 1));
  CHECK(drogon::sync_wait(auditService.compact(
                              {.cutoffMs = cutoffMs,
                               .afterId = firstPage.lastOlderId + 2}))
            .removed == 0);
  const auto secondPage = drogon::sync_wait(auditService.compact(
      {.cutoffMs = cutoffMs, .afterId = firstPage.lastOlderId}));
  CHECK(secondPage.removed == 1);
  CHECK(secondPage.lastOlderId == kBacklogFirstId + 2 * pageSize);
  CHECK(scalar("SELECT COUNT(*) FROM audit_log") ==
        std::to_string(14 + backlogRows / 2));
  CHECK(drogon::sync_wait(auditRepository.findCompactionFrontier()) ==
        kBacklogFirstId + 2 * pageSize);
  CHECK(storedAuditChanges(kBacklogFirstId + backlogRows - 1)["p"]["previous"]
            .asString() == "a");
  CHECK(storedAuditChanges(kBacklogFirstId + backlogRows - 1)["p"]["current"]
            .asString() == "c");
  CHECK(drogon::sync_wait(auditService.compact({.cutoffMs = cutoffMs, .afterId = 0})).removed == 0);
}
