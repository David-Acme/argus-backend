#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/module-gate.hxx>
#include <drogon/drogon.h>
#include <feature/module-impact/services/identity-module-impact.hxx>
#include <feature/module-impact/services/identity-role-reassign.hxx>
#include <sqlite/db-service.hxx>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
ModuleFlag surveillance()
{
  return {.id = "surveillance",
          .enabled = true,
          .lifecycle = "active",
          .dataPurgedAt = 0,
          .roles = {"guard"},
          .name = {.es = "Vigilancia", .en = "Surveillance"},
          .summary = {},
          .intro = {}};
}

struct Database
{
  std::filesystem::path path;
  drogon::orm::DbClientPtr client;

  Database()
      : path(std::filesystem::temp_directory_path() / ("identity-module-impact-" + std::to_string(::getpid()) + ".db"))
  {
    std::filesystem::remove(path);
    client = drogon::orm::DbClient::newSqlite3Client("filename=" + path.string(), 1);
    REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA, client));
    DbService::setIdentityClient(client);
    DbService::freezeClient(path.string());
    client->execSqlSync("INSERT INTO user (id, name, last_name, role, is_active) VALUES "
                        "(1, 'Olga', '', 'owner', 1), (2, 'Raul', 'Paz', 'resident', 1), "
                        "(7, 'Gus', 'Vela', 'guard', 1), (8, 'Gaby', '', 'guard', 0)");
    const auto later = static_cast<int64_t>(std::time(nullptr)) + 600;
    client->execSqlSync("INSERT INTO user_invitation (token_hash, role, max_redemptions, expires_at, created_by) VALUES "
                        "('i-guard', 'guard', 1, ?, 1), ('i-resident', 'resident', 1, ?, 1), "
                        "('i-old', 'guard', 1, 1, 1)",
                        later, later);
  }

  ~Database()
  {
    DbService::setIdentityClient(nullptr);
    client.reset();
    std::error_code error;
    std::filesystem::remove(path, error);
  }

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  [[nodiscard]] std::string roleOf(int64_t id) const
  {
    return client->execSqlSync("SELECT role FROM user WHERE id = ?", id).front()["role"].as<std::string>();
  }
};
}

namespace
{
void reportsHoldersAndInvitations()
{
  const IdentityModuleImpact host;

  const auto report = host.impact("surveillance");
  REQUIRE(report.roleHolders.size() == 2);
  CHECK(report.roleHolders[0].userId == 7);
  CHECK(report.roleHolders[0].name == "Gus");
  CHECK(report.roleHolders[0].lastName == "Vela");
  CHECK(report.roleHolders[0].role == "guard");
  CHECK(report.roleHolders[0].isActive);
  CHECK(report.roleHolders[1].userId == 8);
  CHECK_FALSE(report.roleHolders[1].isActive);
  REQUIRE(report.invitations.size() == 1);
  CHECK(report.invitations[0].role == "guard");
  CHECK(report.invitations[0].createdBy == 1);
  CHECK(report.invitations[0].createdByName == "Olga");
  CHECK(report.invitations[0].expiresAt > std::time(nullptr));
  CHECK(report.stops.empty());

  CHECK(host.impact("productivity").roleHolders.empty());
  CHECK(host.impact("ghost").invitations.empty());
}

void reassignsThroughTheNormalUpdate(const Database& database)
{
  IdentityRoleReassign host;
  const IdentityModuleImpact impactHost;

  const auto refused = host.reassign({.actorUserId = 1, .reassignments = {{.userId = 7, .role = "owner"}}});
  CHECK(refused.status == ReassignStatus::Refused);
  CHECK(refused.reason == "role_invalid");
  CHECK(refused.failedUserId == 7);
  CHECK(database.roleOf(7) == "guard");

  const auto partial = host.reassign(
      {.actorUserId = 1, .reassignments = {{.userId = 7, .role = "resident"}, {.userId = 99, .role = "guest"}}});
  CHECK(partial.status == ReassignStatus::Refused);
  CHECK(partial.applied == 1);
  CHECK(partial.failedUserId == 99);
  CHECK(database.roleOf(7) == "resident");

  const auto done = host.reassign({.actorUserId = 1, .reassignments = {{.userId = 8, .role = "guest"}}});
  CHECK(done.status == ReassignStatus::Applied);
  CHECK(done.applied == 1);
  CHECK(database.roleOf(8) == "guest");
  CHECK(impactHost.impact("surveillance").roleHolders.empty());
}
}

TEST_CASE("the impact of a module names its holders and invitations, and a reassignment moves each holder through the normal update")
{
  const Database database;
  moduleGate().reset();
  moduleGate().apply({surveillance()});
  reportsHoldersAndInvitations();
  reassignsThroughTheNormalUpdate(database);
  moduleGate().reset();
}
