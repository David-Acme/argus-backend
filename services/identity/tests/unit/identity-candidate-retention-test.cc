#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/retention/services/candidate-retention-service.hxx>
#include <sqlite/db-service.hxx>
#include <sync/identity-change-sink.hxx>
#include <sync/module-emit.hxx>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kRetentionDb = "identity-candidate-retention-test.db";
constexpr int64_t kDay = 86400;

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

class RecordingSink : public IdentityChangeSink
{
public:
  [[nodiscard]] drogon::Task<void>
  publishCatalog(const IdentityCatalogInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  emitModule(const ModuleEmitInput& input) const override
  {
    emitted_.push_back(input.body.toJson());
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishModuleAudit(const ModuleAuditInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishUsersAudit(const UserAuditInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAction(const ActionPublishInput&) const override
  {
    co_return;
  }

  [[nodiscard]] const std::vector<Json::Value>& emitted() const { return emitted_; }

private:
  mutable std::vector<Json::Value> emitted_;
};

struct PersonSeed
{
  std::string status;
  int64_t lastSeenAt{0};
  bool linkedUser{false};
};

int64_t seedPerson(const PersonSeed& seed)
{
  const auto client = DbService::client();
  if (seed.linkedUser)
    client->execSqlSync("INSERT OR IGNORE INTO user (id, name, last_name, role, "
                        "lang, is_active) VALUES (1, 'Ana', '', 'owner', 'es', 1)");
  const auto inserted = client->execSqlSync(
      "INSERT INTO person (user_id, status, last_seen_at) VALUES (?, ?, ?)",
      seed.linkedUser ? std::optional<int64_t>{1} : std::optional<int64_t>{},
      seed.status, seed.lastSeenAt);
  const auto id = static_cast<int64_t>(inserted.insertId());
  client->execSqlSync(
      "INSERT INTO face_embedding (person_id, embedding) VALUES (?, x'00')", id);
  client->execSqlSync(
      "INSERT INTO person_snapshot (person_id, image) VALUES (?, x'ffd8')", id);
  client->execSqlSync(
      "INSERT INTO person_tag (person_id, tag) VALUES (?, 'hat')", id);
  return id;
}

bool retired(int64_t personId)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT deleted_at FROM person WHERE id = ?", personId);
  return !rows.empty() && !rows.front()["deleted_at"].isNull();
}

int64_t biometricRows(int64_t personId)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT (SELECT COUNT(*) FROM face_embedding WHERE person_id = ?1) + "
      "(SELECT COUNT(*) FROM person_snapshot WHERE person_id = ?1) + "
      "(SELECT COUNT(*) FROM person_tag WHERE person_id = ?1) AS total",
      personId);
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}
}

TEST_CASE("an unnamed visitor unseen past the Owner's window leaves no biometrics "
          "behind, whatever its status, never reaches the sync rooms, and all "
          "of them go when recognition is off")
{
  std::remove(kRetentionDb);
  std::remove((std::string(kRetentionDb) + "-wal").c_str());
  std::remove((std::string(kRetentionDb) + "-shm").c_str());
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                 .filename = kRetentionDb,
                                 .name = "default",
                                 .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));

  RecordingSink sink;
  identity_change::setSink(&sink);

  const int64_t now = std::time(nullptr);
  const int64_t stale = now - 31 * kDay;
  const int64_t staleCandidate = seedPerson({.status = "candidate",
                                             .lastSeenAt = stale,
                                             .linkedUser = false});
  const int64_t freshCandidate = seedPerson({.status = "candidate",
                                             .lastSeenAt = now - kDay,
                                             .linkedUser = false});
  const int64_t staleKnown = seedPerson({.status = "known",
                                         .lastSeenAt = stale,
                                         .linkedUser = false});
  const int64_t staleLinked = seedPerson({.status = "candidate",
                                          .lastSeenAt = stale,
                                          .linkedUser = true});

  const int64_t namedVisitor = seedPerson({.status = "candidate",
                                           .lastSeenAt = stale,
                                           .linkedUser = false});
  DbService::client()->execSqlSync("UPDATE person SET name = 'Juan' WHERE id = ?",
                                   namedVisitor);
  DbService::client()->execSqlSync(
      "INSERT INTO person_visit (person_id, camera_id, started_at, last_seen_at) "
      "VALUES (?, 6, ?, ?)",
      staleCandidate, stale, stale);

  CandidateRetentionService retention;
  DbService::client()->execSqlSync(
      "UPDATE household_privacy SET visitor_recognition = 1 WHERE id = 1");
  DbService::client()->execSqlSync(
      "UPDATE visitor_setting SET unnamed_retention_days = 60 WHERE id = 1");
  CHECK(drogon::sync_wait(retention.sweep(now)) == 0);
  CHECK_FALSE(retired(staleCandidate));
  DbService::client()->execSqlSync(
      "UPDATE visitor_setting SET unnamed_retention_days = 30 WHERE id = 1");

  CHECK(drogon::sync_wait(retention.sweep(now)) == 2);

  for (const int64_t gone : {staleCandidate, staleKnown}) {
    CAPTURE(gone);
    CHECK(retired(gone));
    CHECK(biometricRows(gone) == 0);
  }
  CHECK(DbService::client()
            ->execSqlSync("SELECT COUNT(*) AS n FROM person_visit WHERE person_id = ?",
                          staleCandidate)
            .front()["n"]
            .as<int64_t>() == 0);
  for (const int64_t kept : {freshCandidate, staleLinked, namedVisitor}) {
    CAPTURE(kept);
    CHECK_FALSE(retired(kept));
    CHECK(biometricRows(kept) == 3);
  }

  CHECK(sink.emitted().empty());

  CHECK(drogon::sync_wait(retention.sweep(now)) == 0);

  DbService::client()->execSqlSync(
      "UPDATE household_privacy SET visitor_recognition = 0 WHERE id = 1");
  CHECK(drogon::sync_wait(retention.sweep(now)) == 1);
  CHECK(retired(freshCandidate));
  for (const int64_t kept : {staleLinked, namedVisitor})
    CHECK_FALSE(retired(kept));
  CHECK(sink.emitted().empty());
  identity_change::setSink(nullptr);
}
