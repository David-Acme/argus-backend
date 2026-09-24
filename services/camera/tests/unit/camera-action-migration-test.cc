#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <feature/actions/repositories/action-command/action-command-repository.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <thread>

namespace
{
constexpr const char* kMigrationDb = "camera-action-migration-test.db";

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

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}

void seedLegacySchema()
{
  auto client = DbService::client();
  client->execSqlSync(
      "CREATE TABLE action_command ("
      "command_id TEXT NOT NULL PRIMARY KEY, "
      "kind TEXT NOT NULL DEFAULT '', camera_id INTEGER NOT NULL DEFAULT 0, "
      "status TEXT NOT NULL DEFAULT 'claimed', "
      "detail TEXT NOT NULL DEFAULT '', "
      "attempts INTEGER NOT NULL DEFAULT 0, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  for (const char* status : {"claimed", "failed", "sent"}) {
    client->execSqlSync(
        "INSERT INTO action_command (command_id, kind, camera_id, status, "
        "detail) VALUES (?, 'announce', 1, ?, ?)",
        std::string("legacy:") + status, status,
        std::string("detail:") + status);
  }
}
}

TEST_CASE("legacy action_command rows migrate fail-closed")
{
  std::remove(kMigrationDb);
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kMigrationDb, "default", -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  seedLegacySchema();
  ActionCommandRepository repository;
  REQUIRE(repository.migrateLegacySchema());

  CHECK(scalar("SELECT status FROM action_command "
               "WHERE command_id = 'legacy:claimed'") == "indeterminate");
  CHECK(scalar("SELECT status FROM action_command "
               "WHERE command_id = 'legacy:failed'") == "indeterminate");
  CHECK(scalar("SELECT status FROM action_command "
               "WHERE command_id = 'legacy:sent'") == "succeeded");

  const ActionClaim indeterminate =
      drogon::sync_wait(repository.claim({.commandId = "legacy:failed",
                                          .kind = "announce",
                                          .cameraId = 1,
                                          .fingerprint = "new-fingerprint",
                                          .at = 1000,
                                          .leaseSeconds = 120}));
  CHECK(indeterminate.kind == ActionClaimKind::Indeterminate);

  const ActionClaim completed = drogon::sync_wait(
      repository.claim({.commandId = "legacy:sent",
                        .kind = "announce",
                        .cameraId = 1,
                        .fingerprint = "different-fingerprint",
                        .at = 1001,
                        .leaseSeconds = 120}));
  CHECK(completed.kind == ActionClaimKind::Completed);

  const ActionClaim fresh =
      drogon::sync_wait(repository.claim({.commandId = "fresh:1",
                                          .kind = "announce",
                                          .cameraId = 1,
                                          .fingerprint = "fp-a",
                                          .at = 1002,
                                          .leaseSeconds = 120}));
  CHECK(fresh.kind == ActionClaimKind::New);
  CHECK(drogon::sync_wait(repository.settle({.commandId = "fresh:1",
                                             .generation = fresh.generation,
                                             .status = "succeeded",
                                             .detail = "done",
                                             .response = {},
                                             .at = 1003})));
  const ActionClaim sameFingerprint =
      drogon::sync_wait(repository.claim({.commandId = "fresh:1",
                                          .kind = "announce",
                                          .cameraId = 1,
                                          .fingerprint = "fp-a",
                                          .at = 1004,
                                          .leaseSeconds = 120}));
  CHECK(sameFingerprint.kind == ActionClaimKind::Completed);
  const ActionClaim changedFingerprint =
      drogon::sync_wait(repository.claim({.commandId = "fresh:1",
                                          .kind = "announce",
                                          .cameraId = 1,
                                          .fingerprint = "fp-b",
                                          .at = 1005,
                                          .leaseSeconds = 120}));
  CHECK(changedFingerprint.kind == ActionClaimKind::Conflict);

  std::remove(kMigrationDb);
  std::remove((std::string(kMigrationDb) + "-wal").c_str());
  std::remove((std::string(kMigrationDb) + "-shm").c_str());
}
