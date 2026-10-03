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
    if (refuse_)
      throw std::runtime_error("the change sink refused the tombstone");
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

  void refuse(bool value) const { refuse_ = value; }
  [[nodiscard]] const std::vector<Json::Value>& emitted() const { return emitted_; }

private:
  mutable bool refuse_{false};
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

TEST_CASE("an unknown face unseen past the window leaves no biometrics behind")
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

  CandidateRetentionService disabled({.candidateDays = 0});
  CHECK(drogon::sync_wait(disabled.sweep(now)) == 0);
  CHECK_FALSE(retired(staleCandidate));

  CandidateRetentionService retention({.candidateDays = 30});
  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(retention.sweep(now)), std::runtime_error);
  sink.refuse(false);
  CHECK_FALSE(retired(staleCandidate));
  CHECK(biometricRows(staleCandidate) == 3);

  CHECK(drogon::sync_wait(retention.sweep(now)) == 1);

  CHECK(retired(staleCandidate));
  CHECK(biometricRows(staleCandidate) == 0);
  for (const int64_t kept : {freshCandidate, staleKnown, staleLinked}) {
    CAPTURE(kept);
    CHECK_FALSE(retired(kept));
    CHECK(biometricRows(kept) == 3);
  }

  REQUIRE(sink.emitted().size() == 1);
  const auto& tombstone = sink.emitted().front();
  CHECK(tombstone["operation"].asInt() == static_cast<int>(SyncOperation::Delete));
  CHECK(tombstone["info"]["id"].asInt64() == staleCandidate);
  CHECK(tombstone["info"]["deletedAt"].asInt64() >= now);
  CHECK(tombstone["info"].size() == 2);

  CHECK(drogon::sync_wait(retention.sweep(now)) == 0);
  identity_change::setSink(nullptr);
}
