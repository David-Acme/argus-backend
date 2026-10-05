#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/user-role.hxx>
#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <feature/transport/dtos/socket-frame-dto.hxx>
#include <feature/transport/infra/cached-user-directory.hxx>
#include <feature/transport/infra/sync-socket-registrar.hxx>
#include <feature/transport/services/connection-lanes.hxx>
#include <feature/transport/services/frame-lane.hxx>
#include <feature/transport/services/sync-service.hxx>
#include <feature/transport/services/synchronized-service.hxx>
#include <nats/nats-subject.hxx>
#include <shared/services/room/room-manager.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <sync/module-audit-event.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-change.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <sync/user-audit-event.hxx>
#include <text/json-util.hxx>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>
#include <vector>

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
  if (!module) {
    FAIL("expected a value in module");
    return;
  }
  CHECK(module->emit.operation == SyncOperation::Add);
  CHECK(module->users == std::nullopt);
  CHECK(moduleRoom(module->emit.option) == moduleRoom(TableName::Camera));

  const Json::Value userEmit = json_util::fromString(
      R"({"operation":4,"option":"user","info":{},"users":[42,43]})");
  const auto user = sync_fan_out::parseEvent(userEmit);
  if (!user) {
    FAIL("expected a value in user");
    return;
  }
  if (!user->users) {
    FAIL("expected a value in user->users");
    return;
  }
  CHECK(user->users->size() == 2);
  CHECK(userRoom((*user->users)[0]) == userRoom(42));

  const Json::Value userEmitEmpty = json_util::fromString(
      R"({"operation":4,"option":"user","info":{},"users":[]})");
  const auto empty = sync_fan_out::parseEvent(userEmitEmpty);
  if (!empty) {
    FAIL("expected a value in empty");
    return;
  }
  if (!empty->users) {
    FAIL("expected a value in empty->users");
    return;
  }
  CHECK(empty->users->empty());
  CHECK(empty->user == std::nullopt);

  const Json::Value disconnect = json_util::fromString(
      R"({"operation":7,"option":"user","info":{},"users":[7],"user":7,"action":"disconnect"})");
  const auto disconnection = sync_fan_out::parseEvent(disconnect);
  if (!disconnection) {
    FAIL("expected a value in disconnection");
    return;
  }
  CHECK(disconnection->user == 7);

  const auto replacement = sync_fan_out::parseEvent(
      sync_change::roleRoomsPayload({.userId = 7,
                                     .oldRole = userRoleToString(UserRole::Resident),
                                     .newRole = userRoleToString(UserRole::Guest)}));
  if (!replacement) {
    FAIL("expected a value in replacement");
    return;
  }
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

TEST_CASE("fan-out refuses what it cannot route instead of guessing the user room")
{
  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString(
      R"({"operation":4,"option":"camera_snapshot","info":{"id":3}})")));
  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString(
      R"({"operation":4,"option":"notification","info":{},"users":["42"]})")));
  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString(
      R"({"operation":7,"option":"user","info":{},"user":"7","action":"disconnect"})")));
  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString(
      R"({"operation":7,"option":"user","info":{},"user":7,"action":"replace_role_rooms","old_role":{},"new_role":"guest"})")));
  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString(
      R"({"operation":7,"option":"user","info":{},"user":7,"action":"disconnect_session"})")));
  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString(
      R"({"operation":7,"option":"user","info":{},"user":7,"session":"","action":"disconnect_session"})")));
  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString(
      R"({"operation":7,"option":"user","info":{},"user":7,"session":42,"action":"disconnect_session"})")));
  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString(
      R"({"operation":7,"option":"user","info":{},"session":"abc","action":"disconnect_session"})")));

  const auto filtered = sync_fan_out::parseEvent(json_util::fromString(
      R"({"operation":4,"option":"notification","info":{},"users":[-999,0,42]})"));
  if (!filtered) {
    FAIL("expected a value in filtered");
    return;
  }
  if (!filtered->users) {
    FAIL("expected a value in filtered->users");
    return;
  }
  CHECK(*filtered->users == std::vector<int64_t>{42});
}

TEST_CASE("audit events outside the vocabulary are refused")
{
  const auto event = [](std::string_view table, int priority) {
    Json::Value json;
    json["record_id"] = Json::Int64{5};
    json["table_name"] = std::string(table);
    json["changes"] = Json::objectValue;
    json["event_timestamp"] = Json::Int64{1790000000};
    json["priority"] = priority;
    return json;
  };
  CHECK(ModuleAuditEvent::fromJson(event("project", 2)));
  CHECK_FALSE(ModuleAuditEvent::fromJson(event("project", 7)));
  CHECK_FALSE(ModuleAuditEvent::fromJson(event("no_such_table", 1)));

  auto scoped = event("project_task", 1);
  scoped["users"].append(Json::Int64{42});
  CHECK(UserAuditEvent::fromJson(scoped));
  scoped["users"].append("43");
  CHECK_FALSE(UserAuditEvent::fromJson(scoped));
}

TEST_CASE("a socket frame must be an object, and its type is read without throwing")
{
  CHECK_FALSE(SocketFrameDto::fromJson(json_util::fromString("[]")));
  CHECK_FALSE(SocketFrameDto::fromJson(json_util::fromString("5")));
  CHECK_FALSE(SocketFrameDto::fromJson(json_util::fromString(R"("sync")")));

  const auto objectType = SocketFrameDto::fromJson(json_util::fromString(R"({"type":{}})"));
  if (!objectType) {
    FAIL("expected a value in objectType");
    return;
  }
  CHECK(objectType->type.empty());

  const auto sync = SocketFrameDto::fromJson(json_util::fromString(R"({"type":"sync","payload":{}})"));
  if (!sync) {
    FAIL("expected a value in sync");
    return;
  }
  CHECK(sync->type == "sync");
}

TEST_CASE("control actions are accepted only from the feed that owns them")
{
  CHECK(sync_fan_out::controlScopeOf(nats_subject::kAuthSession) ==
        sync_fan_out::ControlScope::Session);
  CHECK(sync_fan_out::controlScopeOf(nats_subject::kCameraChange) ==
        sync_fan_out::ControlScope::None);
  CHECK(sync_fan_out::controlScopeOf(nats_subject::kIdentityChange) ==
        sync_fan_out::ControlScope::None);

  SocketEmitDto frame;
  frame.operation = SyncOperation::AuthContextChanged;
  frame.option = TableName::User;
  frame.obj = Json::Value(Json::objectValue);
  const Json::Value session = sync_change::disconnectSessionPayload(
      frame, {.userId = 7, .sessionId = "0123456789abcdef"});
  const Json::Value disconnect = sync_change::disconnectPayload(frame, 7);
  const Json::Value rooms = sync_change::roleRoomsPayload(
      {.userId = 7, .oldRole = "guest", .newRole = "owner"});
  const Json::Value emit = sync_change::userEmitPayload(frame, {7});

  using sync_fan_out::ControlScope;
  CHECK_FALSE(sync_fan_out::parseEvent(session, ControlScope::None));
  CHECK_FALSE(sync_fan_out::parseEvent(disconnect, ControlScope::None));
  CHECK_FALSE(sync_fan_out::parseEvent(rooms, ControlScope::None));
  CHECK(sync_fan_out::parseEvent(emit, ControlScope::None));

  CHECK(sync_fan_out::parseEvent(session, ControlScope::Session));
  CHECK_FALSE(sync_fan_out::parseEvent(disconnect, ControlScope::Session));
  CHECK_FALSE(sync_fan_out::parseEvent(rooms, ControlScope::Session));
  CHECK(sync_fan_out::parseEvent(emit, ControlScope::Session));

  CHECK(sync_fan_out::parseEvent(session, ControlScope::All));
  CHECK(sync_fan_out::parseEvent(disconnect, ControlScope::All));
  CHECK(sync_fan_out::parseEvent(rooms, ControlScope::All));
}

TEST_CASE("a socket lane runs its frames one at a time and rate limits the rest")
{
  FrameLane lane({.burst = 3.0, .refillPerSecond = 1.0, .maxQueued = 3},
                 {.userId = 7, .loop = nullptr});
  const auto frame = [](std::string type) {
    return FrameJob{.kind = FrameJobKind::Frame,
                    .message = Json::Value(Json::objectValue),
                    .raw = {},
                    .type = std::move(type)};
  };

  CHECK(lane.admit(frame("a"), 100.0) == FrameAdmission::Start);
  CHECK(lane.draining());
  CHECK(lane.admit(frame("b"), 100.0) == FrameAdmission::Queued);
  CHECK(lane.admit(frame("c"), 100.0) == FrameAdmission::Queued);
  CHECK(lane.admit(frame("d"), 100.0) == FrameAdmission::Refused);

  CHECK(lane.next()->type == "a");
  CHECK(lane.next()->type == "b");
  CHECK(lane.admit(frame("e"), 100.5) == FrameAdmission::Refused);
  CHECK(lane.admit(frame("f"), 101.0) == FrameAdmission::Queued);
  CHECK(lane.admitRevalidation() == FrameAdmission::Queued);
  CHECK(lane.admitRevalidation() == FrameAdmission::Refused);
  CHECK(lane.next()->type == "c");
  CHECK(lane.next()->type == "f");
  CHECK(lane.next()->kind == FrameJobKind::Revalidate);
  CHECK_FALSE(lane.next().has_value());
  CHECK_FALSE(lane.draining());

  CHECK(lane.admitRevalidation() == FrameAdmission::Start);
  CHECK(lane.next()->kind == FrameJobKind::Revalidate);
  CHECK_FALSE(lane.next().has_value());
  CHECK(lane.admit(frame("g"), 1000.0) == FrameAdmission::Start);
  CHECK(lane.userId() == 7);
}

namespace
{
class CountingDirectory final : public IUserDirectory
{
public:
  drogon::Task<DirectoryLookup> lookup(int64_t userId) const override
  {
    ++calls;
    if (unavailable)
      co_return DirectoryLookup{};
    co_return DirectoryLookup{
        .status = DirectoryLookupStatus::Found,
        .user = DirectoryUser{.id = userId,
                              .name = "Ana",
                              .lastName = "Garcia",
                              .lang = "es",
                              .role = role,
                              .isActive = active}};
  }

  mutable int calls{0};
  bool unavailable{false};
  bool active{true};
  UserRole role{UserRole::Resident};
};

class ClosingConnection final : public drogon::WebSocketConnection
{
public:
  void send(const char* msg, uint64_t len, const drogon::WebSocketMessageType) override
  {
    messages.emplace_back(msg, len);
  }
  void send(std::string_view msg, const drogon::WebSocketMessageType) override
  {
    messages.emplace_back(msg);
  }
  void sendJson(const Json::Value& json, const drogon::WebSocketMessageType) override
  {
    messages.push_back(json_util::toString(json));
  }
  const trantor::InetAddress& localAddr() const override { return addr_; }
  const trantor::InetAddress& peerAddr() const override { return addr_; }
  bool connected() const override { return closeReason.empty(); }
  bool disconnected() const override { return !closeReason.empty(); }
  void shutdown(const drogon::CloseCode, const std::string& reason) override { closeReason = reason; }
  void forceClose() override { closeReason = "forced"; }
  void setPingMessage(const std::string&, const std::chrono::duration<double>&) override {}
  void disablePing() override {}

  std::vector<std::string> messages;
  std::string closeReason;

private:
  trantor::InetAddress addr_{"127.0.0.1", 0};
};
}

TEST_CASE("the directory cache answers within its window and forgets on an identity change")
{
  const auto inner = std::make_shared<CountingDirectory>();
  auto now = std::chrono::steady_clock::time_point{};
  const CachedUserDirectory cache(
      inner, {.ttl = std::chrono::seconds(10), .clock = [&now] { return now; }});

  CHECK(drogon::sync_wait(cache.lookup(7)).user->role == UserRole::Resident);
  CHECK(drogon::sync_wait(cache.lookup(7)).user->role == UserRole::Resident);
  CHECK(inner->calls == 1);

  inner->role = UserRole::Guest;
  now += std::chrono::seconds(9);
  CHECK(drogon::sync_wait(cache.lookup(7)).user->role == UserRole::Resident);
  cache.forget(7);
  CHECK(drogon::sync_wait(cache.lookup(7)).user->role == UserRole::Guest);
  CHECK(inner->calls == 2);

  now += std::chrono::seconds(10);
  CHECK(drogon::sync_wait(cache.lookup(7)).status == DirectoryLookupStatus::Found);
  CHECK(inner->calls == 3);

  inner->unavailable = true;
  now += std::chrono::seconds(10);
  CHECK(drogon::sync_wait(cache.lookup(7)).status ==
        DirectoryLookupStatus::Unavailable);
  CHECK(drogon::sync_wait(cache.lookup(7)).status ==
        DirectoryLookupStatus::Unavailable);
  CHECK(inner->calls == 5);
}

TEST_CASE("revalidation reaches every socket of one user, once while one is pending")
{
  ConnectionLanes lanes;
  std::vector<int64_t> started;
  lanes.setDrainStarter([&started](const drogon::WebSocketConnectionPtr&,
                                   const std::shared_ptr<FrameLane>& lane) {
    started.push_back(lane->userId());
  });
  const auto first = std::make_shared<ClosingConnection>();
  const auto second = std::make_shared<ClosingConnection>();
  const auto other = std::make_shared<ClosingConnection>();
  const drogon::WebSocketConnectionPtr firstConn = first;
  const drogon::WebSocketConnectionPtr secondConn = second;
  const drogon::WebSocketConnectionPtr otherConn = other;
  lanes.open({.conn = firstConn, .userId = 7, .loop = nullptr});
  lanes.open({.conn = secondConn, .userId = 7, .loop = nullptr});
  lanes.open({.conn = otherConn, .userId = 8, .loop = nullptr});

  CHECK(lanes.connectedUsers() == std::vector<int64_t>{7, 8});
  CHECK(lanes.revalidateUser(7) == 2);
  CHECK(started == std::vector<int64_t>{7, 7});
  CHECK_FALSE(lanes.drained());
  CHECK(lanes.revalidateUser(7) == 2);
  CHECK(started.size() == 2);

  for (const auto& conn : {firstConn, secondConn}) {
    const auto lane = lanes.find(conn);
    REQUIRE(lane);
    CHECK(lane->next()->kind == FrameJobKind::Revalidate);
    CHECK_FALSE(lane->next().has_value());
  }
  CHECK(lanes.drained());
  lanes.close(firstConn);
  CHECK(lanes.find(firstConn) == nullptr);
  CHECK(lanes.revalidateUser(7) == 1);
}

TEST_CASE("a socket whose account was disabled is closed by its revalidation")
{
  const auto directory = std::make_shared<CountingDirectory>();
  const auto lanes = std::make_shared<ConnectionLanes>();
  SyncService service;
  service.setUserDirectory(directory);
  service.setLanes(lanes);

  const auto socket = std::make_shared<ClosingConnection>();
  const drogon::WebSocketConnectionPtr conn = socket;
  conn->setContext(std::make_shared<JwtContext>(JwtContext{.sub = 7,
                                                           .name = "Ana",
                                                           .role = UserRole::Resident,
                                                           .isActive = true,
                                                           .deviceHash = "device",
                                                           .sessionId = "session"}));
  REQUIRE(service.laneFor(conn));

  CHECK(lanes->revalidateUser(7) == 1);
  CHECK(socket->closeReason.empty());
  CHECK(directory->calls == 1);

  directory->unavailable = true;
  CHECK(lanes->revalidateUser(7) == 1);
  CHECK(socket->closeReason.empty());

  directory->unavailable = false;
  directory->active = false;
  CHECK(lanes->revalidateUser(7) == 1);
  CHECK(socket->closeReason == "account_disabled");
  CHECK(lanes->drained());
}

TEST_CASE("the event table is answered empty, in the shape the app pages")
{
  const SynchronizedService service;
  SynchronizedDto body;
  SynchronizedBodyDto event;
  event.requiredCreate = true;
  event.requiredDeleted = true;
  event.findLastCreated = true;
  body.event = event;
  const JwtContext owner{.sub = 1,
                         .name = "Owner",
                         .role = UserRole::Owner,
                         .isActive = true,
                         .deviceHash = "device",
                         .sessionId = "session"};
  const Json::Value frame = drogon::sync_wait(service.sync(body, owner));
  const Json::Value& node = frame["info"]["event"];
  CHECK(node["created"].isArray());
  CHECK(node["created"].empty());
  CHECK(node["deleted"].isArray());
  CHECK(node["deleted"].empty());
  CHECK(node["lastSyncDate"].isObject());
  CHECK(node["lastSyncDate"].empty());

  body.event->findLastCreated = false;
  const Json::Value plain = drogon::sync_wait(service.sync(body, owner));
  CHECK_FALSE(plain["info"]["event"].isMember("lastSyncDate"));
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
  if (!module) {
    FAIL("expected a value in module");
    return;
  }
  const sync_fan_out::FanOutPlan modulePlan = sync_fan_out::planEvent(*module);
  CHECK(modulePlan.kind == sync_fan_out::FanOutPlan::Kind::ModuleEmit);
  CHECK(modulePlan.room == moduleRoom(TableName::Camera));
  CHECK(modulePlan.rooms.empty());

  const auto scoped =
      sync_fan_out::parseEvent(sync_change::userEmitPayload(
          emit(SyncOperation::Add, TableName::Notification), {42, 43}));
  if (!scoped) {
    FAIL("expected a value in scoped");
    return;
  }
  const sync_fan_out::FanOutPlan scopedPlan = sync_fan_out::planEvent(*scoped);
  CHECK(scopedPlan.kind == sync_fan_out::FanOutPlan::Kind::UserEmit);
  REQUIRE(scopedPlan.rooms.size() == 2);
  CHECK(scopedPlan.rooms[0] == userRoom(42));
  CHECK(scopedPlan.rooms[1] == userRoom(43));

  const auto unscoped =
      sync_fan_out::parseEvent(sync_change::userEmitPayload(
          emit(SyncOperation::Add, TableName::Notification), {}));
  if (!unscoped) {
    FAIL("expected a value in unscoped");
    return;
  }
  const sync_fan_out::FanOutPlan unscopedPlan = sync_fan_out::planEvent(*unscoped);
  CHECK(unscopedPlan.kind == sync_fan_out::FanOutPlan::Kind::UserEmit);
  CHECK(unscopedPlan.rooms.empty());

  const auto disconnection =
      sync_fan_out::parseEvent(sync_change::disconnectPayload(
          emit(SyncOperation::AuthContextChanged, TableName::User), 42));
  if (!disconnection) {
    FAIL("expected a value in disconnection");
    return;
  }
  const sync_fan_out::FanOutPlan disconnectPlan = sync_fan_out::planEvent(*disconnection);
  CHECK(disconnectPlan.kind == sync_fan_out::FanOutPlan::Kind::Disconnect);
  CHECK(disconnectPlan.userId == 42);

  const auto sessionDisconnection = sync_fan_out::parseEvent(
      sync_change::disconnectSessionPayload(
          emit(SyncOperation::AuthContextChanged, TableName::User),
          {.userId = 42, .sessionId = "0123456789abcdef0123456789abcdef"}));
  if (!sessionDisconnection) {
    FAIL("a session disconnect payload did not parse");
    return;
  }
  const sync_fan_out::FanOutPlan sessionPlan =
      sync_fan_out::planEvent(*sessionDisconnection);
  CHECK(sessionPlan.kind == sync_fan_out::FanOutPlan::Kind::DisconnectSession);
  CHECK(sessionPlan.userId == 42);
  CHECK(sessionPlan.sessionId == "0123456789abcdef0123456789abcdef");
  CHECK(sessionPlan.rooms.empty());

  const auto replacement = sync_fan_out::parseEvent(
      sync_change::roleRoomsPayload({.userId = 42,
                                     .oldRole = userRoleToString(UserRole::Resident),
                                     .newRole = userRoleToString(UserRole::Guest)}));
  if (!replacement) {
    FAIL("expected a value in replacement");
    return;
  }
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
  if (!event) {
    FAIL("expected a value in event");
    return;
  }

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
