#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/jwt-filter.hxx>
#include <auth/module-gate.hxx>
#include <auth/request-context.hxx>
#include <drogon/drogon.h>
#include <feature/user/controllers/user-controller.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>

#include <cstdio>
#include <filesystem>
#include <string>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
ModuleFlag surveillance(bool enabled)
{
  return {.id = "surveillance",
          .enabled = enabled,
          .lifecycle = enabled ? "active" : "disabled",
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
      : path(std::filesystem::temp_directory_path() /
             ("identity-user-update-" + std::to_string(::getpid()) + ".db"))
  {
    std::filesystem::remove(path);
    client = drogon::orm::DbClient::newSqlite3Client("filename=" + path.string(), 1);
    REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA, client));
    DbService::setIdentityClient(client);
    DbService::freezeClient(path.string());
    client->execSqlSync("INSERT INTO user (id, name, last_name, role) VALUES "
                        "(1, 'Orsa', '', 'owner'), (2, 'Raul', '', 'resident')");
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
};

Json::Value patchRole(UserController& controller, const std::string& role)
{
  Json::Value body(Json::objectValue);
  body["role"] = role;
  auto req = drogon::HttpRequest::newHttpJsonRequest(body);
  req->attributes()->insert(AuthContext::kJwtKey,
                            JwtContext{.sub = 1,
                                       .name = "Orsa",
                                       .role = UserRole::Owner,
                                       .isActive = true,
                                       .deviceHash = "d",
                                       .sessionId = "s"});
  const auto response = drogon::sync_wait(controller.update(req, 2));
  CHECK(response->getStatusCode() == drogon::k200OK);
  return json_util::fromString(std::string(response->body()));
}
}

TEST_CASE("a role change to a role whose module is off succeeds and says the role is inactive")
{
  const Database database;
  UserController controller;

  moduleGate().reset();
  moduleGate().apply({surveillance(false)});
  const auto inactive = patchRole(controller, "guard");
  CHECK(inactive["info"]["role"] == "guard");
  CHECK(inactive["info"]["roleActive"] == false);
  CHECK(database.client->execSqlSync("SELECT role FROM user WHERE id = 2").front()["role"].as<std::string>() ==
        "guard");

  const auto active = patchRole(controller, "resident");
  CHECK(active["info"]["role"] == "resident");
  CHECK(active["info"]["roleActive"] == true);

  moduleGate().apply({surveillance(true)});
  CHECK(patchRole(controller, "guard")["info"]["roleActive"] == true);
  moduleGate().reset();
}
