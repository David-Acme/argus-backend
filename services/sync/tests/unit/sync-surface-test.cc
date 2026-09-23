#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/user-role.hxx>
#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <feature/transport/infra/sync-socket-registrar.hxx>
#include <shared/services/room/room-manager.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-change.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>

namespace
{

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

void exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "exec failed";
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
  sqlite3_free(error);
}

DbHandle openFile(const std::string& path)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
    const std::string message = raw ? sqlite3_errmsg(raw) : "open failed";
    sqlite3_close_v2(raw);
    throw std::runtime_error(message);
  }
  return {raw, sqlite3_close_v2};
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

TEST_CASE("fan-out parses the sync-change wire contract")
{
  const Json::Value moduleEmit = json_util::fromString(
      R"({"operation":4,"option":"camera","info":{"id":3}})");
  const auto module = sync_fan_out::parseEvent(moduleEmit);
  REQUIRE(module);
  CHECK(module->emit.operation == SyncOperation::Add);
  CHECK(module->users == std::nullopt);
  CHECK(moduleRoom(module->emit.option) == moduleRoom(TableName::Camera));

  const Json::Value userEmit = json_util::fromString(
      R"({"operation":4,"option":"user","info":{},"users":[42,43]})");
  const auto user = sync_fan_out::parseEvent(userEmit);
  REQUIRE(user);
  REQUIRE(user->users);
  CHECK(user->users->size() == 2);
  CHECK(userRoom((*user->users)[0]) == userRoom(42));

  const Json::Value userEmitEmpty = json_util::fromString(
      R"({"operation":4,"option":"user","info":{},"users":[]})");
  const auto empty = sync_fan_out::parseEvent(userEmitEmpty);
  REQUIRE(empty);
  REQUIRE(empty->users);
  CHECK(empty->users->empty());
  CHECK(empty->user == std::nullopt);

  const Json::Value disconnect = json_util::fromString(
      R"({"operation":7,"option":"user","info":{},"users":[7],"user":7,"action":"disconnect"})");
  const auto disconnection = sync_fan_out::parseEvent(disconnect);
  REQUIRE(disconnection);
  CHECK(disconnection->user == 7);

  const auto replacement = sync_fan_out::parseEvent(
      sync_change::roleRoomsPayload({.userId = 7,
                                     .oldRole = userRoleToString(UserRole::Resident),
                                     .newRole = userRoleToString(UserRole::Guest)}));
  REQUIRE(replacement);
  CHECK(replacement->emit.operation == SyncOperation::AuthContextChanged);
  CHECK(replacement->emit.option == TableName::User);
  CHECK(replacement->user == 7);
  CHECK(replacement->oldRole == UserRole::Resident);
  CHECK(replacement->newRole == UserRole::Guest);

  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString(
      R"({"option":"camera","info":{}})")));
  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString("null")));
  CHECK_FALSE(sync_fan_out::parseEvent(
      json_util::fromString(R"({"action":"disconnect","operation":7,"option":"user","info":{}})")));
}

TEST_CASE("identity change events never fan out to the client sockets")
{
  const Json::Value identity = json_util::fromString(
      R"({"kind":"identity","table":"person","id":7,"deleted":false,
          "row":{"id":7,"user_id":42,"name":"Ana Garcia"}})");
  CHECK(sync_fan_out::parseEvent(identity) == std::nullopt);

  const Json::Value tombstone = json_util::fromString(
      R"({"kind":"identity","table":"person","id":7,"deleted":true,
          "row":{}})");
  CHECK(sync_fan_out::parseEvent(tombstone) == std::nullopt);
}

TEST_CASE("fan-out routing table matches the legacy SocketService mapping")
{
  const auto emit = [](SyncOperation operation, TableName table) {
    SocketEmitDto body;
    body.operation = operation;
    body.option = table;
    body.obj["id"] = 7;
    return body;
  };

  const auto module =
      sync_fan_out::parseEvent(sync_change::emitPayload(emit(SyncOperation::Add,
                                                             TableName::Camera)));
  REQUIRE(module);
  const sync_fan_out::FanOutPlan modulePlan = sync_fan_out::planEvent(*module);
  CHECK(modulePlan.kind == sync_fan_out::FanOutPlan::Kind::ModuleEmit);
  CHECK(modulePlan.room == moduleRoom(TableName::Camera));
  CHECK(modulePlan.rooms.empty());

  const auto scoped =
      sync_fan_out::parseEvent(sync_change::userEmitPayload(
          emit(SyncOperation::Add, TableName::Notification), {42, 43}));
  REQUIRE(scoped);
  const sync_fan_out::FanOutPlan scopedPlan = sync_fan_out::planEvent(*scoped);
  CHECK(scopedPlan.kind == sync_fan_out::FanOutPlan::Kind::UserEmit);
  REQUIRE(scopedPlan.rooms.size() == 2);
  CHECK(scopedPlan.rooms[0] == userRoom(42));
  CHECK(scopedPlan.rooms[1] == userRoom(43));

  const auto unscoped =
      sync_fan_out::parseEvent(sync_change::userEmitPayload(
          emit(SyncOperation::Add, TableName::Notification), {}));
  REQUIRE(unscoped);
  const sync_fan_out::FanOutPlan unscopedPlan = sync_fan_out::planEvent(*unscoped);
  CHECK(unscopedPlan.kind == sync_fan_out::FanOutPlan::Kind::UserEmit);
  CHECK(unscopedPlan.rooms.empty());

  const auto disconnection =
      sync_fan_out::parseEvent(sync_change::disconnectPayload(
          emit(SyncOperation::AuthContextChanged, TableName::User), 42));
  REQUIRE(disconnection);
  const sync_fan_out::FanOutPlan disconnectPlan = sync_fan_out::planEvent(*disconnection);
  CHECK(disconnectPlan.kind == sync_fan_out::FanOutPlan::Kind::Disconnect);
  CHECK(disconnectPlan.userId == 42);

  const auto replacement = sync_fan_out::parseEvent(
      sync_change::roleRoomsPayload({.userId = 42,
                                     .oldRole = userRoleToString(UserRole::Resident),
                                     .newRole = userRoleToString(UserRole::Guest)}));
  REQUIRE(replacement);
  const sync_fan_out::FanOutPlan replacePlan = sync_fan_out::planEvent(*replacement);
  CHECK(replacePlan.kind == sync_fan_out::FanOutPlan::Kind::ReplaceRoleRooms);
  CHECK(replacePlan.replaceInput.userId == 42);
  CHECK(replacePlan.replaceInput.oldRole == UserRole::Resident);
  CHECK(replacePlan.replaceInput.newRole == UserRole::Guest);
}

TEST_CASE("fan-out re-emits the exact legacy wire triple")
{
  const Json::Value payload = json_util::fromString(
      R"({"operation":5,"option":"reminder","info":{"id":9,"title":"x"},"users":[]})");
  const auto event = sync_fan_out::parseEvent(payload);
  REQUIRE(event);

  SocketEmitDto body;
  body.operation = SyncOperation::Delete;
  body.option = TableName::Reminder;
  Json::Value info;
  info["id"] = 9;
  info["title"] = "x";
  body.obj = info;

  CHECK(json_util::toString(event->emit.toJson()) ==
        json_util::toString(body.toJson()));
}

TEST_CASE("read-only legacy database rejects writes and serves reads")
{
  const char* dbPath = "sync-surface-test-readonly.db";
  std::remove(dbPath);

  DbService::enableUriFilenames();
  {
    const DbHandle db = openFile(dbPath);
    exec(db.get(),
         "CREATE TABLE note (id INTEGER PRIMARY KEY, text TEXT NOT NULL)");
    exec(db.get(), "INSERT INTO note (text) VALUES ('hello')");
  }

  const auto readOnly = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=file:") + dbPath + "?mode=ro", 1);
  DbService::setReadOnlyClient(readOnly);

  const auto rows = readOnly->execSqlSync("SELECT text FROM note");
  REQUIRE(rows.size() == 1);
  CHECK(rows.front()["text"].as<std::string>() == "hello");

  bool writeFailed = false;
  try {
    readOnly->execSqlSync("INSERT INTO note (text) VALUES ('nope')");
  }
  catch (const std::exception&) {
    writeFailed = true;
  }
  CHECK(writeFailed);

  drain(readOnly);
  DbService::setReadOnlyClient(nullptr);
  std::remove(dbPath);
}

TEST_CASE("sync surface registers the socket and both filters")
{
  const char* path = "sync-surface-test-config.toml";
  {
    std::ofstream file(path);
    file << "[jwt]\n"
         << "secret = \"0123456789abcdef0123456789abcdef0123456789\"\n"
         << "refresh_secret = \"fedcba9876543210fedcba9876543210fedcba98\"\n"
         << "access_ttl_minutes = 15\n"
         << "refresh_ttl_days = 30\n";
  }

  ConfigService::load(path);
  std::remove(path);

  const SyncRegistrationStats stats = registerSyncSurface({});

  CHECK(stats.controllers == 1);
  CHECK(stats.filters == 2);
}
