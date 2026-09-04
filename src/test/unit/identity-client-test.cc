#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/services/sqlite/db-service.hxx>

// The fallback to DbService::client() when no identity client is installed is
// the same one-line path readOnlyClient() uses; it is exercised on the booted
// legacy by the A/B probe matrix instead of an in-process default client.

TEST_CASE("installed identity client serves its own database")
{
  DbService::setIdentityClient(nullptr);

  const char* dbPath = "identity-client-test.db";
  std::remove(dbPath);

  DbService::enableUriFilenames();
  auto writable = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=") + dbPath, 1);
  writable->execSqlSync("CREATE TABLE marker (id INTEGER PRIMARY KEY)");
  writable->execSqlSync("INSERT INTO marker (id) VALUES (42)");
  writable.reset();

  DbService::setIdentityClient(drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=file:") + dbPath + "?mode=ro", 1));

  const auto rows = DbService::identityClient()->execSqlSync(
      "SELECT id FROM marker");
  REQUIRE(rows.size() == 1);
  CHECK(rows.front()["id"].as<int64_t>() == 42);

  bool writeFailed = false;
  try {
    DbService::identityClient()->execSqlSync(
        "INSERT INTO marker (id) VALUES (43)");
  }
  catch (const std::exception&) {
    writeFailed = true;
  }
  CHECK(writeFailed);

  DbService::setIdentityClient(nullptr);
  std::remove(dbPath);
}