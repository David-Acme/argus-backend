#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <guard-repository.hxx>
#include <guard-schema.hxx>
#include <shared/services/sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

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

constexpr const char* kLegacyEncounter =
    "CREATE TABLE guard_encounter ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "person_id INTEGER NOT NULL DEFAULT 0, "
    "signature TEXT NOT NULL DEFAULT '', "
    "state TEXT NOT NULL DEFAULT 'observing' "
    "CHECK (state IN ('observing', 'assessing', 'escalating', 'closed')), "
    "grade TEXT NOT NULL DEFAULT 'none', "
    "checks INTEGER NOT NULL DEFAULT 0, "
    "best_camera_id INTEGER NOT NULL DEFAULT 0, "
    "best_score REAL NOT NULL DEFAULT 0, "
    "first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL, "
    "last_action_at INTEGER NOT NULL DEFAULT 0)";

constexpr const char* kLegacyAction =
    "CREATE TABLE guard_action ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "incident_id INTEGER NOT NULL DEFAULT 0, "
    "camera_id INTEGER NOT NULL DEFAULT 0, "
    "person_id INTEGER NOT NULL DEFAULT 0, "
    "kind TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT '', "
    "detail TEXT NOT NULL DEFAULT '', "
    "created_at INTEGER NOT NULL DEFAULT 0)";

constexpr const char* kLegacyInbox =
    "CREATE TABLE guard_observation_inbox ("
    "event_id TEXT NOT NULL PRIMARY KEY, "
    "camera_id INTEGER NOT NULL DEFAULT 0, "
    "observation_id TEXT NOT NULL DEFAULT '', "
    "received_at INTEGER NOT NULL DEFAULT 0)";

constexpr const char* kLegacyGuest =
    "CREATE TABLE guard_expected_guest ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "description TEXT NOT NULL DEFAULT '', valid_from INTEGER NOT NULL, "
    "valid_until INTEGER NOT NULL, "
    "created_at INTEGER NOT NULL DEFAULT 0, deleted_at INTEGER)";

constexpr const char* kLegacyOutbox =
    "CREATE TABLE guard_action_outbox ("
    "command_id TEXT NOT NULL PRIMARY KEY, "
    "encounter_id INTEGER NOT NULL DEFAULT 0, "
    "incident_id INTEGER NOT NULL DEFAULT 0, "
    "camera_id INTEGER NOT NULL DEFAULT 0, "
    "person_id INTEGER NOT NULL DEFAULT 0, "
    "kind TEXT NOT NULL DEFAULT '', "
    "status TEXT NOT NULL DEFAULT 'pending' "
    "CHECK (status IN ('pending', 'in_flight', 'sent', 'denied', 'failed', "
    "'unknown')), "
    "detail TEXT NOT NULL DEFAULT '', "
    "response TEXT NOT NULL DEFAULT '{}', "
    "payload TEXT NOT NULL DEFAULT '', "
    "attempts INTEGER NOT NULL DEFAULT 0, "
    "created_at INTEGER NOT NULL DEFAULT 0, "
    "updated_at INTEGER NOT NULL DEFAULT 0)";

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}

void seedLegacyDb(const std::string& path)
{
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
                                              1);
  client->execSqlSync(kLegacyEncounter);
  client->execSqlSync(kLegacyAction);
  client->execSqlSync(kLegacyInbox);
  client->execSqlSync(kLegacyGuest);
  client->execSqlSync(kLegacyOutbox);
  client->execSqlSync(
      "INSERT INTO guard_action_outbox (command_id, encounter_id, "
      "incident_id, camera_id, person_id, kind, status, detail, response, "
      "payload, attempts, created_at, updated_at) VALUES "
      "('legacy-sent', 0, 0, 1, 0, 'notify', 'sent', '', '{}', '', 1, 100, "
      "100), "
      "('legacy-denied', 0, 0, 1, 0, 'notify', 'denied', '', '{}', '', 1, 100, "
      "100), "
      "('legacy-failed', 0, 0, 1, 0, 'notify', 'failed', '', '{}', '', 2, 100, "
      "100), "
      "('legacy-unknown', 0, 0, 1, 0, 'notify', 'unknown', '', '{}', '', 1, "
      "100, 100), "
      "('legacy-pending', 0, 0, 1, 0, 'notify', 'pending', '', '{}', '', 0, "
      "100, 100)");
  client->execSqlSync(
      "INSERT INTO guard_encounter (person_id, signature, state, grade, "
      "checks, best_camera_id, best_score, first_seen, last_seen) "
      "VALUES (5, 'sig', 'assessing', 'medium', 3, 1, 12.5, 100, 140)");
  client->execSqlSync(
      "INSERT INTO guard_action (incident_id, camera_id, person_id, kind, "
      "status, detail, created_at) VALUES (1, 1, 5, 'notify', 'sent', '', 100)");
  client->execSqlSync(
      "INSERT INTO guard_observation_inbox (event_id, camera_id, "
      "observation_id, received_at) VALUES ('old-event', 1, 'obs', 100)");
}
} // namespace

TEST_CASE("a legacy guard database migrates in place without data loss")
{
  const TempDb db("guard-migration-test");
  seedLegacyDb(db.path());
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  REQUIRE(guard_schema::migrate(ARGUS_GUARD_SCHEMA_PATH));

  CHECK(scalar("SELECT state FROM guard_encounter WHERE id = 1") ==
        "verifying");
  CHECK(scalar("SELECT person_id FROM guard_encounter WHERE id = 1") == "5");
  CHECK(scalar("SELECT grade FROM guard_encounter WHERE id = 1") == "medium");
  CHECK(scalar("SELECT COUNT(*) FROM guard_encounter") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_action") == "1");
  CHECK(scalar("SELECT status FROM guard_observation_inbox "
               "WHERE event_id = 'old-event'") == "processing");

  REQUIRE(DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH));

  const auto columns = [](const std::string& table) {
    return scalar("SELECT COUNT(*) FROM pragma_table_info('" + table + "')");
  };
  CHECK(std::stoi(columns("guard_encounter")) >= 16);
  CHECK(std::stoi(columns("guard_observation_inbox")) >= 12);
  CHECK(std::stoi(columns("guard_expected_guest")) >= 10);
  CHECK(std::stoi(columns("guard_action")) >= 10);
  CHECK(std::stoi(columns("guard_incident")) >= 10);
  CHECK(std::stoi(columns("guard_assessment")) >= 10);
  CHECK(scalar("SELECT stage FROM guard_observation_inbox "
               "WHERE event_id = 'old-event'") == "0");
  CHECK(scalar("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
               "AND name = 'guard_dead_letter'") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' "
               "AND name = 'idx_guard_incident_event'") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_encounter_transition") == "0");
  CHECK(scalar("SELECT COUNT(*) FROM guard_action_outbox") == "5");
  CHECK(scalar("SELECT status FROM guard_action_outbox "
               "WHERE command_id = 'legacy-sent'") == "succeeded");
  CHECK(scalar("SELECT status FROM guard_action_outbox "
               "WHERE command_id = 'legacy-denied'") == "rejected");
  CHECK(scalar("SELECT status FROM guard_action_outbox "
               "WHERE command_id = 'legacy-failed'") == "retryable_failed");
  CHECK(scalar("SELECT status FROM guard_action_outbox "
               "WHERE command_id = 'legacy-unknown'") == "rejected");
  CHECK(scalar("SELECT status FROM guard_action_outbox "
               "WHERE command_id = 'legacy-pending'") == "pending");
  CHECK(scalar("SELECT COUNT(*) FROM guard_action_outbox WHERE status NOT IN "
               "('pending', 'in_flight', 'retryable_failed', 'succeeded', "
               "'duplicate_succeeded', 'rejected', 'conflict', "
               "'indeterminate')") == "0");
  const GuardRepository repository;
  const auto replayed = drogon::sync_wait(repository.planIntent(
      {.commandId = "legacy-pending",
       .encounterId = 0,
       .incidentId = 0,
       .cameraId = 1,
       .personId = 0,
       .kind = "notify",
       .payload = {},
       .at = 200}));
  REQUIRE(replayed.has_value());
  CHECK(replayed->status == GuardIntentStatus::Pending);
  CHECK(guardIntentStatusIsResumable(replayed->status));
  CHECK(scalar("SELECT COUNT(*) FROM guard_evidence") == "0");

  DbService::client()->execSqlSync(
      "UPDATE guard_encounter SET state = 'closed' WHERE id = 1");
  CHECK(scalar("SELECT state FROM guard_encounter WHERE id = 1") == "closed");

  drogon::app().quit();
  runner.join();
}
