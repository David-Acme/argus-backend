#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <json/value.h>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <thread>
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

class AppRunner
{
public:
  AppRunner()
      : finished_(std::make_shared<std::atomic<bool>>(false)),
        runner_([flag = finished_] {
          drogon::app().run();
          flag->store(true, std::memory_order_release);
        })
  {
  }

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !finished_->load(std::memory_order_acquire) &&
                    !drogon::app().isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    if (finished_->load(std::memory_order_acquire)) {
      runner_.join();
      return;
    }
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::shared_ptr<std::atomic<bool>> finished_;
  std::thread runner_;
};

bool loopIsRunning()
{
  return drogon::app().isRunning() && drogon::app().getLoop()->isRunning();
}

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (loopIsRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return loopIsRunning();
}

bool waitUntil(const std::function<bool()>& ready,
               std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return ready();
}

}

TEST_CASE("installed identity client serves its own database")
{
  DbService::setIdentityClient(nullptr);

  const char* dbPath = "identity-client-test.db";
  std::remove(dbPath);

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

TEST_CASE("the frozen client serves a statement after the app's clients are reset")
{
  const std::string dbPath = "frozen-client-test.db";
  std::remove(dbPath.c_str());
  std::remove((dbPath + "-wal").c_str());
  std::remove((dbPath + "-shm").c_str());
  seedDb(dbPath.c_str());

  Json::Value config(Json::objectValue);
  Json::Value clients(Json::arrayValue);
  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  clients.append(client);
  config["db_clients"] = clients;
  Json::Value listeners(Json::arrayValue);
  Json::Value listener(Json::objectValue);
  listener["address"] = "127.0.0.1";
  listener["port"] = 0;
  listeners.append(listener);
  config["listeners"] = listeners;
  drogon::app().loadConfigJson(config);

  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  const auto appClient = DbService::client();
  REQUIRE(appClient != nullptr);

  DbService::freezeClient(dbPath);
  drogon::app().quit();
  const auto closed = [&appClient] {
    return !appClient->hasAvailableConnections();
  };
  REQUIRE(waitUntil(closed, std::chrono::seconds(10)));

  const auto frozen = DbService::client();
  REQUIRE(frozen != nullptr);
  CHECK(frozen.get() != appClient.get());
  CHECK(frozen->hasAvailableConnections());

  const auto rows = frozen->execSqlSync("SELECT id FROM marker");
  REQUIRE(rows.size() == 1);
  CHECK(rows.front()["id"].as<int64_t>() == 42);

  frozen->execSqlSync("INSERT INTO marker (id) VALUES (43)");
  const auto counted = frozen->execSqlSync("SELECT count(*) AS n FROM marker");
  REQUIRE(counted.size() == 1);
  CHECK(counted.front()["n"].as<int64_t>() == 2);

  std::remove(dbPath.c_str());
  std::remove((dbPath + "-wal").c_str());
  std::remove((dbPath + "-shm").c_str());
}

int main(int argc, char** argv)
{
  DbService::enableUriFilenames();
  doctest::Context context(argc, argv);
  return context.run();
}
