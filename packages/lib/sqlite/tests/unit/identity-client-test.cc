#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <trantor/net/EventLoop.h>

namespace
{

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

void exec(sqlite3* db, const char* sql)
{
  char* error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "exec failed";
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
  sqlite3_free(error);
}

void seedDb(const char* path)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open(path, &raw) != SQLITE_OK) {
    const std::string message = raw ? sqlite3_errmsg(raw) : "open failed";
    sqlite3_close_v2(raw);
    throw std::runtime_error(message);
  }
  const DbHandle db(raw, sqlite3_close_v2);
  exec(db.get(), "CREATE TABLE marker (id INTEGER PRIMARY KEY)");
  exec(db.get(), "INSERT INTO marker (id) VALUES (42)");
}

void drain(const drogon::orm::DbClientPtr& client)
{
  auto drained = std::make_shared<std::promise<void>>();
  auto done = drained->get_future();
  client->execSqlAsync(
      "SELECT 1",
      [drained](const drogon::orm::Result&) {
        trantor::EventLoop::getEventLoopOfCurrentThread()->queueInLoop(
            [drained]() { drained->set_value(); });
      },
      [drained](const std::exception_ptr& e) {
        try {
          std::rethrow_exception(e);
        }
        catch (const std::exception& ex) {
          std::fprintf(stderr, "drain statement failed: %s\n", ex.what());
        }
        drained->set_value();
      });
  if (done.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    throw std::runtime_error("the client's loop did not drain");
}

}

TEST_CASE("installed identity client serves its own database")
{
  DbService::setIdentityClient(nullptr);

  const char* dbPath = "identity-client-test.db";
  std::remove(dbPath);

  DbService::enableUriFilenames();
  seedDb(dbPath);

  auto readOnly = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=file:") + dbPath + "?mode=ro", 1);
  DbService::setIdentityClient(readOnly);

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

  drain(readOnly);
  DbService::setIdentityClient(nullptr);
  readOnly.reset();
  std::remove(dbPath);
}
