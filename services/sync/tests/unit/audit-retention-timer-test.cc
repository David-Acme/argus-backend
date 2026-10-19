#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/fanout/services/audit-retention-service.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
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

template <typename Predicate>
bool waitFor(std::chrono::milliseconds timeout, Predicate ready)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return ready();
}

inline constexpr int64_t kMsPerDay = 86'400'000;
inline constexpr int kRetentionDays = 7;
inline constexpr int kStartWindowIgnoredDays = 30;

struct AuditRowSeed
{
  int64_t id{0};
  int64_t recordId{0};
  const char* changes{"{}"};
  int64_t eventTimestamp{0};
};

void seedAuditRow(const AuditRowSeed& row)
{
  DbService::client()->execSqlSync(
      "INSERT INTO audit_log (id, record_id, table_name, changes, priority, "
      "event_timestamp) VALUES (" +
      std::to_string(row.id) + ", " + std::to_string(row.recordId) +
      ", 'camera', '" + row.changes + "', 1, " +
      std::to_string(row.eventTimestamp) + ")");
}

struct UserAuditRowSeed
{
  int64_t id{0};
  int64_t userId{0};
  int64_t recordId{0};
  const char* changes{"{}"};
  int64_t eventTimestamp{0};
};

void seedUserAuditRow(const UserAuditRowSeed& row)
{
  DbService::client()->execSqlSync(
      "INSERT INTO user_audit_log (id, user_id, record_id, table_name, "
      "changes, priority, event_timestamp) VALUES (" +
      std::to_string(row.id) + ", " + std::to_string(row.userId) + ", " +
      std::to_string(row.recordId) + ", 'camera', '" + row.changes + "', 1, " +
      std::to_string(row.eventTimestamp) + ")");
}

Json::Value storedChanges(const std::string& table, int64_t id)
{
  return json_util::fromString(scalar("SELECT changes FROM " + table +
                                      " WHERE id = " + std::to_string(id)));
}

std::string rowCount(const std::string& table, int64_t id)
{
  return scalar("SELECT COUNT(*) FROM " + table +
                " WHERE id = " + std::to_string(id));
}
}

TEST_CASE("the retention timer folds old audit rows, advances the frontier "
          "and spares same-key young rows")
{
  const TempDb db("audit-retention-timer-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                 .filename = db.path(),
                                 .name = "default",
                                 .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  REQUIRE(DbService::runScriptFile(ARGUS_SYNC_SCHEMA_PATH));

  const AuditLogRepository auditRepository;
  const UserAuditLogRepository userAuditRepository;
  const int64_t nowMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t outsideMs = nowMs - 8 * kMsPerDay;
  const int64_t halfOutsideMs = nowMs - 7 * kMsPerDay - kMsPerDay / 2;
  const int64_t halfInsideMs = nowMs - 6 * kMsPerDay - kMsPerDay / 2;
  const int64_t insideMs = nowMs - kMsPerDay;

  seedAuditRow({.id = 1,
                .recordId = 1,
                .changes = R"({"name":{"previous":"Front","current":"Front door"}})",
                .eventTimestamp = outsideMs});
  seedAuditRow({.id = 2,
                .recordId = 1,
                .changes = R"({"name":{"previous":"Front door","current":"Porch"}})",
                .eventTimestamp = outsideMs + 1000});
  seedAuditRow({.id = 3,
                .recordId = 2,
                .changes = R"({"mode":{"previous":"off","current":"on"}})",
                .eventTimestamp = halfOutsideMs});
  seedAuditRow({.id = 4,
                .recordId = 2,
                .changes = R"({"mode":{"previous":"on","current":"auto"}})",
                .eventTimestamp = halfInsideMs});
  seedAuditRow({.id = 5,
                .recordId = 3,
                .changes = R"({"label":{"previous":"gate","current":"door"}})",
                .eventTimestamp = insideMs});

  seedUserAuditRow(
      {.id = 1,
       .userId = 7,
       .recordId = 1,
       .changes = R"({"name":{"previous":"Ann","current":"Anne"}})",
       .eventTimestamp = outsideMs});
  seedUserAuditRow(
      {.id = 2,
       .userId = 7,
       .recordId = 1,
       .changes = R"({"name":{"previous":"Anne","current":"Annie"}})",
       .eventTimestamp = outsideMs + 1000});
  seedUserAuditRow(
      {.id = 3,
       .userId = 7,
       .recordId = 1,
       .changes = R"({"name":{"previous":"Annie","current":"Ana"}})",
       .eventTimestamp = insideMs});

  CHECK(drogon::sync_wait(auditRepository.findCompactionFrontier()) == 0);
  CHECK(drogon::sync_wait(userAuditRepository.findCompactionFrontier()) == 0);

  AuditRetentionService retention;
  retention.start(kRetentionDays);
  retention.start(kStartWindowIgnoredDays);

  REQUIRE(waitFor(std::chrono::seconds(60), [] {
    return scalar("SELECT COUNT(*) FROM audit_log WHERE id = 1") == "0";
  }));
  REQUIRE(waitFor(std::chrono::seconds(10), [] {
    return scalar("SELECT COUNT(*) FROM user_audit_log WHERE id = 1") == "0";
  }));
  REQUIRE(waitFor(std::chrono::milliseconds(5000),
                  [&retention] { return retention.drained(); }));

  CHECK(rowCount("audit_log", 2) == "1");
  CHECK(rowCount("audit_log", 3) == "1");
  CHECK(rowCount("audit_log", 4) == "1");
  CHECK(rowCount("audit_log", 5) == "1");
  const Json::Value folded = storedChanges("audit_log", 2);
  CHECK(folded["name"]["previous"].asString() == "Front");
  CHECK(folded["name"]["current"].asString() == "Porch");
  const Json::Value spared = storedChanges("audit_log", 4);
  CHECK(spared.isMember("mode"));
  CHECK(spared["mode"]["previous"].asString() == "on");
  CHECK(spared["mode"]["current"].asString() == "auto");
  const Json::Value sparedOlder = storedChanges("audit_log", 3);
  CHECK(sparedOlder["mode"]["current"].asString() == "on");
  CHECK(drogon::sync_wait(auditRepository.findCompactionFrontier()) == 1);

  CHECK(rowCount("user_audit_log", 2) == "1");
  CHECK(rowCount("user_audit_log", 3) == "1");
  const Json::Value foldedUser = storedChanges("user_audit_log", 2);
  CHECK(foldedUser["name"]["previous"].asString() == "Ann");
  CHECK(foldedUser["name"]["current"].asString() == "Annie");
  const Json::Value sparedUser = storedChanges("user_audit_log", 3);
  CHECK(sparedUser["name"]["previous"].asString() == "Annie");
  CHECK(sparedUser["name"]["current"].asString() == "Ana");
  CHECK(drogon::sync_wait(userAuditRepository.findCompactionFrontier()) == 1);

  retention.stop();
  CHECK(retention.drained());
}
