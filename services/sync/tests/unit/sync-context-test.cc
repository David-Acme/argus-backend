#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/module-gate.hxx>
#include <auth/request-context.hxx>
#include <auth/role-access.hxx>
#include <auth/user-role.hxx>
#include <drogon/HttpRequest.h>
#include <drogon/WebSocketConnection.h>
#include <drogon/utils/coroutine.h>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <feature/transport/infra/productivity-sync-source.hxx>
#include <feature/transport/services/sync-service.hxx>
#include <feature/transport/services/synchronized-service.hxx>
#include <shared/services/context/user-context.hxx>
#include <shared/services/room/room-manager.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-operation.hxx>
#include <text/json-util.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
const std::optional<routes::RemoteLinkState> kNoRemote;

class FakeConnection final : public drogon::WebSocketConnection
{
public:
  void send(const char* msg, uint64_t len, const drogon::WebSocketMessageType) override
  {
    messages.emplace_back(msg, len);
  }
  void send(std::string_view msg, const drogon::WebSocketMessageType) override { messages.emplace_back(msg); }
  void sendJson(const Json::Value& json, const drogon::WebSocketMessageType) override
  {
    messages.push_back(json_util::toString(json));
  }
  [[nodiscard]] const trantor::InetAddress& localAddr() const override { return addr_; }
  [[nodiscard]] const trantor::InetAddress& peerAddr() const override { return addr_; }
  [[nodiscard]] bool connected() const override { return !closed; }
  [[nodiscard]] bool disconnected() const override { return closed; }
  void shutdown(const drogon::CloseCode, const std::string&) override { closed = true; }
  void forceClose() override { closed = true; }
  void setPingMessage(const std::string&, const std::chrono::duration<double>&) override {}
  void disablePing() override {}

  [[nodiscard]] Json::Value last() const { return json_util::fromString(messages.back()); }

  std::vector<std::string> messages;
  bool closed{false};

private:
  trantor::InetAddress addr_{"127.0.0.1", 0};
};

ModuleFlag moduleOf(std::string id, bool enabled, std::vector<std::string> roles)
{
  ModuleFlag flag;
  flag.id = std::move(id);
  flag.enabled = enabled;
  flag.lifecycle = enabled ? "active" : "disabled";
  flag.roles = std::move(roles);
  flag.kind = flag.id == "core" ? "core" : "available";
  flag.name = {.es = "Nombre " + flag.id, .en = "Name " + flag.id};
  flag.summary = {.es = "Resumen", .en = "Summary"};
  flag.intro = {.es = {.what = "Qué es", .examples = {"uno", "dos", "tres"}},
                .en = {.what = "What it is", .examples = {"one", "two", "three"}}};
  return flag;
}

ModuleSnapshot snapshotWith(bool surveillance, bool productivity)
{
  return ModuleSnapshot({moduleOf("core", true, {}),
                         moduleOf("surveillance", surveillance, {"guard"}),
                         moduleOf("productivity", productivity, {})});
}

void setGate(bool surveillance, bool productivity)
{
  moduleGate().reset();
  moduleGate().apply(snapshotWith(surveillance, productivity).modules());
}

JwtContext contextOf(int64_t id, UserRole role)
{
  return JwtContext{.sub = id,
                    .name = "Ana",
                    .role = role,
                    .isActive = true,
                    .deviceHash = "device",
                    .sessionId = "session"};
}

std::shared_ptr<FakeConnection> connect(SyncService& service, int64_t id, UserRole role)
{
  auto conn = std::make_shared<FakeConnection>();
  auto req = drogon::HttpRequest::newHttpRequest();
  req->getAttributes()->insert(AuthContext::kJwtKey, contextOf(id, role));
  std::atomic<bool> done{false};
  drogon::async_run([&]() -> drogon::Task<> {
    co_await service.handleConnect(req, conn);
    done = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!done && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(done.load());
  return conn;
}

class FakeProductivitySource final : public ProductivitySyncSource
{
public:
  [[nodiscard]] bool serves(ProductivitySyncTable) const override { return true; }

  [[nodiscard]] std::unique_ptr<Syncable> sourceFor(ProductivitySyncTable, const JwtContext& ctx) const override
  {
    return std::make_unique<Rows>(ctx.sub);
  }

private:
  class Rows final : public Syncable
  {
  public:
    explicit Rows(int64_t userId) : userId_(userId) {}

    [[nodiscard]] drogon::Task<std::vector<Json::Value>>
    find(const SyncFilter&) const override
    {
      Json::Value row(Json::objectValue);
      row["id"] = 1;
      row["userId"] = static_cast<Json::Int64>(userId_);
      co_return std::vector<Json::Value>{row};
    }
    [[nodiscard]] drogon::Task<std::vector<Json::Value>>
    findDeleted(const SyncFilter&) const override
    {
      co_return std::vector<Json::Value>{};
    }
    [[nodiscard]] drogon::Task<std::optional<Json::Value>>
    findLast(const SyncFilter&) const override
    {
      co_return std::nullopt;
    }
    [[nodiscard]] drogon::Task<std::optional<Json::Value>>
    findLastDeleted(const SyncFilter&) const override
    {
      co_return std::nullopt;
    }

  private:
    int64_t userId_;
  };
};

bool contains(const Json::Value& list, std::string_view text)
{
  return std::ranges::any_of(list, [&](const Json::Value& item) { return item.asString() == text; });
}

void leave(const std::shared_ptr<FakeConnection>& conn)
{
  RoomManager{}.leaveAll(conn);
}
}

TEST_CASE("the context carries the role, its capabilities, the roles to offer and every module in both languages")
{
  const auto modules = snapshotWith(true, true);
  const Json::Value none;
  const Json::Value context = user_context::build({.userId = 7, .role = UserRole::Resident, .modules = modules, .ownerCatalog = none, .remote = kNoRemote});

  CHECK(context["userId"] == 7);
  CHECK(context["role"] == "resident");
  CHECK(context["roleActive"] == true);
  CHECK(contains(context["capabilities"], "camera.view"));
  CHECK(contains(context["capabilities"], "agenda.write"));
  CHECK(contains(context["capabilities"], "reminders.read"));
  CHECK_FALSE(contains(context["capabilities"], "users.manage"));
  CHECK_FALSE(context.isMember("ownerCatalog"));

  REQUIRE(context["roles"].size() == 4);
  CHECK(context["roles"][0]["id"] == "owner");
  CHECK(context["roles"][0]["module"] == "core");
  CHECK(context["roles"][0]["active"] == true);
  CHECK(context["roles"][2]["id"] == "guard");
  CHECK(context["roles"][2]["module"] == "surveillance");
  CHECK(context["roles"][2]["active"] == true);

  REQUIRE(context["modules"].size() == 3);
  const Json::Value& surveillance = context["modules"][1];
  CHECK(surveillance["id"] == "surveillance");
  CHECK(surveillance["kind"] == "available");
  CHECK(context["modules"][0]["kind"] == "core");
  CHECK(surveillance["name"]["es"] == "Nombre surveillance");
  CHECK(surveillance["name"]["en"] == "Name surveillance");
  CHECK(surveillance["summary"]["en"] == "Summary");
  CHECK(surveillance["intro"]["es"]["what"] == "Qué es");
  CHECK(surveillance["intro"]["en"]["examples"].size() == 3);
  CHECK(surveillance["roles"][0] == "guard");
  CHECK(surveillance["enabled"] == true);
  CHECK(surveillance["lifecycle"] == "active");
  CHECK(surveillance["dataPurgedAt"].isNull());
}

TEST_CASE("a role whose module is off is inactive and holds the baseline alone")
{
  const auto modules = snapshotWith(false, true);
  const Json::Value none;
  const Json::Value context = user_context::build({.userId = 9, .role = UserRole::Guard, .modules = modules, .ownerCatalog = none, .remote = kNoRemote});
  CHECK(context["role"] == "guard");
  CHECK(context["roleActive"] == false);
  CHECK(contains(context["capabilities"], "safety.panic"));
  CHECK(contains(context["capabilities"], "reminders.write"));
  CHECK_FALSE(contains(context["capabilities"], "camera.view"));
  CHECK_FALSE(contains(context["capabilities"], "directory.read"));
  CHECK(context["roles"][2]["active"] == false);
  CHECK(context["modules"][1]["enabled"] == false);
  CHECK(context["modules"][1]["lifecycle"] == "disabled");
}

TEST_CASE("a role this build does not know gets an empty context, never a default role")
{
  const auto modules = snapshotWith(true, true);
  const Json::Value catalog = json_util::fromString(R"([{"id":"core"}])");
  const Json::Value context = user_context::build({.userId = 3, .role = UserRole::Unknown, .modules = modules, .ownerCatalog = catalog, .remote = kNoRemote});
  CHECK(context["role"] == "unknown");
  CHECK(context["roleActive"] == false);
  CHECK(context["capabilities"].empty());
  CHECK_FALSE(context.isMember("ownerCatalog"));
}

TEST_CASE("only the Owner receives the owner catalog")
{
  const auto modules = snapshotWith(true, true);
  const Json::Value catalog = json_util::fromString(R"([{"id":"core","job":null}])");
  const Json::Value owner = user_context::build({.userId = 1, .role = UserRole::Owner, .modules = modules, .ownerCatalog = catalog, .remote = kNoRemote});
  CHECK(owner["ownerCatalog"].isArray());
  CHECK(owner["ownerCatalog"][0]["id"] == "core");
  for (const auto role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    const Json::Value other = user_context::build({.userId = 2, .role = role, .modules = modules, .ownerCatalog = catalog, .remote = kNoRemote});
    CHECK_FALSE(other.isMember("ownerCatalog"));
  }
  const Json::Value none;
  CHECK_FALSE(user_context::build({.userId = 1, .role = UserRole::Owner, .modules = modules, .ownerCatalog = none, .remote = kNoRemote})
                  .isMember("ownerCatalog"));
}

TEST_CASE("the context update frame is operation 13 with the bare context as info")
{
  const auto modules = snapshotWith(true, true);
  const Json::Value none;
  const Json::Value context = user_context::build({.userId = 1, .role = UserRole::Guest, .modules = modules, .ownerCatalog = none, .remote = kNoRemote});
  const Json::Value frame = user_context::updateFrame(context);
  CHECK(frame["operation"] == 13);
  CHECK(frame["option"] == "user");
  CHECK(frame["info"] == context);
  CHECK(syncOperationToString(SyncOperation::ContextUpdate) == "context_update");
}

TEST_CASE("the first frame of a socket is InitialInfo with the whole context, and rooms follow the modules")
{
  setGate(true, true);
  SyncService service;

  const auto resident = connect(service, 21, UserRole::Resident);
  REQUIRE_FALSE(resident->messages.empty());
  const Json::Value first = json_util::fromString(resident->messages.front());
  CHECK(first["operation"] == 0);
  CHECK(first["info"]["id"] == 21);
  CHECK(first["info"]["role"] == "resident");
  CHECK(first["info"]["context"]["userId"] == 21);
  CHECK(first["info"]["context"]["roleActive"] == true);
  CHECK(contains(first["info"]["context"]["capabilities"], "camera.view"));
  CHECK(RoomManager{}.isOnline(moduleRoom(TableName::Camera)));
  CHECK(RoomManager{}.isOnline(moduleRoom(TableName::Project)));
  leave(resident);
  CHECK_FALSE(RoomManager{}.isOnline(moduleRoom(TableName::Camera)));

  setGate(false, true);
  const auto guard = connect(service, 22, UserRole::Guard);
  const Json::Value inactive = json_util::fromString(guard->messages.front());
  CHECK(inactive["info"]["context"]["roleActive"] == false);
  CHECK(contains(inactive["info"]["context"]["capabilities"], "safety.panic"));
  CHECK_FALSE(contains(inactive["info"]["context"]["capabilities"], "camera.view"));
  CHECK_FALSE(RoomManager{}.isOnline(moduleRoom(TableName::Camera)));
  CHECK(RoomManager{}.isOnline(userRoom(22)));
  leave(guard);
  moduleGate().reset();
}

TEST_CASE("a socket connecting before the enabled set is known gets no context, never a module list from an unknown gate")
{
  moduleGate().reset();
  SyncService service;

  const auto resident = connect(service, 61, UserRole::Resident);
  REQUIRE_FALSE(resident->messages.empty());
  const Json::Value first = json_util::fromString(resident->messages.front());
  CHECK(first["operation"] == 0);
  CHECK(first["info"]["id"] == 61);
  CHECK(first["info"]["role"] == "resident");
  CHECK(first["info"]["isActive"] == true);
  CHECK_FALSE(first["info"].isMember("context"));
  leave(resident);

  const auto owner = connect(service, 62, UserRole::Owner);
  CHECK_FALSE(json_util::fromString(owner->messages.front())["info"].isMember("context"));
  leave(owner);

  const Json::Value none;
  const Json::Value unknown = user_context::build(
      {.userId = 61, .role = UserRole::Resident, .modules = moduleGate().snapshot(), .ownerCatalog = none, .remote = kNoRemote});
  CHECK(unknown["modules"].empty());
  CHECK_FALSE(unknown["capabilities"].empty());

  moduleGate().apply(snapshotWith(true, true).modules());
  const auto late = connect(service, 63, UserRole::Resident);
  const Json::Value known = json_util::fromString(late->messages.front());
  REQUIRE(known["info"]["context"].isObject());
  CHECK(known["info"]["context"]["userId"] == 63);
  CHECK(contains(known["info"]["context"]["capabilities"], "agenda.read"));
  const Json::Value& modules = known["info"]["context"]["modules"];
  REQUIRE(modules.size() == 3);
  CHECK(std::ranges::any_of(modules, [](const Json::Value& module) {
    return module["id"] == "productivity" && module["enabled"] == true;
  }));
  leave(late);
  moduleGate().reset();
}

TEST_CASE("a socket that connected while the set was unknown is told by the context update and moves its rooms")
{
  moduleGate().reset();
  SyncService service;
  const auto resident = connect(service, 71, UserRole::Resident);
  CHECK_FALSE(json_util::fromString(resident->messages.front())["info"].isMember("context"));
  resident->messages.clear();

  const auto modules = snapshotWith(true, true);
  const Json::Value none;
  moduleGate().apply(modules.modules());
  userContext().deliverLocal(resident, {.modules = modules, .ownerCatalog = none});

  REQUIRE(resident->messages.size() == 1);
  const Json::Value update = resident->last();
  CHECK(update["operation"] == 13);
  CHECK(update["info"]["userId"] == 71);
  REQUIRE(update["info"]["modules"].size() == 3);
  CHECK(contains(update["info"]["capabilities"], "camera.view"));
  CHECK(RoomManager{}.isOnline(moduleRoom(TableName::Camera)));
  CHECK(RoomManager{}.isOnline(moduleRoom(TableName::Project)));
  leave(resident);
  moduleGate().reset();
}

TEST_CASE("the Owner's InitialInfo carries the owner catalog when settings answers and goes without it when not")
{
  setGate(true, true);
  SyncService service;

  userContext().setCatalogFetch([] { return std::optional<std::string>(R"([{"id":"surveillance","job":null}])"); });
  const auto owner = connect(service, 1, UserRole::Owner);
  const Json::Value info = json_util::fromString(owner->messages.front());
  CHECK(info["info"]["context"]["ownerCatalog"][0]["id"] == "surveillance");
  leave(owner);

  userContext().setCatalogFetch([] { return std::optional<std::string>(); });
  const auto unreachable = connect(service, 1, UserRole::Owner);
  CHECK_FALSE(json_util::fromString(unreachable->messages.front())["info"]["context"].isMember("ownerCatalog"));
  leave(unreachable);

  userContext().setCatalogFetch([] { return std::optional<std::string>("not json"); });
  const auto garbled = connect(service, 1, UserRole::Owner);
  CHECK_FALSE(json_util::fromString(garbled->messages.front())["info"]["context"].isMember("ownerCatalog"));
  leave(garbled);

  userContext().setCatalogFetch([] { return std::optional<std::string>(R"([{"id":"x"}])"); });
  const auto resident = connect(service, 2, UserRole::Resident);
  CHECK_FALSE(json_util::fromString(resident->messages.front())["info"]["context"].isMember("ownerCatalog"));
  leave(resident);

  userContext().setCatalogFetch({});
  moduleGate().reset();
}

TEST_CASE("a module change moves the socket's rooms and sends it a context update, data untouched")
{
  setGate(true, true);
  SyncService service;
  const auto resident = connect(service, 31, UserRole::Resident);
  const auto guard = connect(service, 32, UserRole::Guard);
  CHECK(RoomManager{}.isOnline(moduleRoom(TableName::Camera)));
  CHECK(RoomManager{}.isOnline(moduleRoom(TableName::Project)));
  resident->messages.clear();
  guard->messages.clear();

  const auto off = snapshotWith(false, true);
  const Json::Value none;
  userContext().deliverLocal(resident, {.modules = off, .ownerCatalog = none});
  userContext().deliverLocal(guard, {.modules = off, .ownerCatalog = none});

  REQUIRE(resident->messages.size() == 1);
  const Json::Value update = resident->last();
  CHECK(update["operation"] == 13);
  CHECK(update["info"]["role"] == "resident");
  CHECK(update["info"]["roleActive"] == true);
  CHECK_FALSE(contains(update["info"]["capabilities"], "camera.view"));
  CHECK(contains(update["info"]["capabilities"], "agenda.read"));
  CHECK(guard->last()["info"]["roleActive"] == false);
  CHECK_FALSE(RoomManager{}.isOnline(moduleRoom(TableName::Camera)));
  CHECK(RoomManager{}.isOnline(moduleRoom(TableName::Project)));
  CHECK(RoomManager{}.isOnline(userRoom(31)));

  const auto on = snapshotWith(true, true);
  userContext().deliverLocal(resident, {.modules = on, .ownerCatalog = none});
  CHECK(resident->last()["info"]["capabilities"].isArray());
  CHECK(contains(resident->last()["info"]["capabilities"], "camera.view"));
  CHECK(RoomManager{}.isOnline(moduleRoom(TableName::Camera)));
  leave(resident);
  leave(guard);
  moduleGate().reset();
}

TEST_CASE("a role change swaps the socket's role rooms without leaking the old ones")
{
  setGate(true, true);
  SyncService service;
  const auto socket = connect(service, 41, UserRole::Resident);
  const auto modules = moduleGate().snapshot();
  CHECK(RoomManager{}.isOnline(roleRoom(UserRole::Resident)));
  CHECK(RoomManager{}.isOnline(moduleRoom(TableName::Project)));

  RoomManager{}.reconcileRoleRooms(socket, {.role = UserRole::Guest, .modules = modules});
  CHECK(RoomManager{}.isOnline(roleRoom(UserRole::Guest)));
  CHECK_FALSE(RoomManager{}.isOnline(roleRoom(UserRole::Resident)));
  CHECK_FALSE(RoomManager{}.isOnline(moduleRoom(TableName::Project)));
  CHECK_FALSE(RoomManager{}.isOnline(moduleRoom(TableName::Camera)));
  CHECK(RoomManager{}.isOnline(reducedModuleRoom(TableName::Camera)));
  CHECK(RoomManager{}.isOnline(kConnectedRoom));
  CHECK(RoomManager{}.isOnline(userRoom(41)));

  RoomManager{}.reconcileRoleRooms(socket, {.role = UserRole::Unknown, .modules = modules});
  CHECK_FALSE(RoomManager{}.isOnline(roleRoom(UserRole::Guest)));
  CHECK_FALSE(RoomManager{}.isOnline(reducedModuleRoom(TableName::Camera)));
  CHECK(RoomManager{}.isOnline(userRoom(41)));
  CHECK(RoomManager{}.isOnline(kConnectedRoom));
  leave(socket);
  moduleGate().reset();
}

TEST_CASE("reminders never get a module room for any role, and live only in the user's own room")
{
  setGate(true, true);
  for (const auto role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(userRoleToString(role));
    const auto rooms = roleRoomsOf(role, moduleGate().snapshot());
    CHECK(std::ranges::find(rooms, moduleRoom(TableName::Reminder)) == rooms.end());
    CHECK(std::ranges::find(rooms, moduleRoom(TableName::ReminderDetail)) == rooms.end());
    const auto tables = SynchronizedService::auditTablesFor(role, moduleGate().snapshot());
    CHECK(std::ranges::find(tables, TableName::Reminder) == tables.end());
    CHECK(std::ranges::find(tables, TableName::ReminderDetail) == tables.end());
  }
  moduleGate().reset();
}

TEST_CASE("a module that is off stops its tables from syncing and its diffs from being paged, data kept")
{
  const FakeProductivitySource productivity;
  SynchronizedService service;
  service.setProductivitySource(&productivity);
  SynchronizedDto body;
  SynchronizedBodyDto pull;
  pull.requiredCreate = true;
  body.project = pull;
  body.calendarEvent = pull;
  body.event = pull;
  body.reminder = pull;
  body.reminderDetail = pull;

  for (const auto role : {UserRole::Owner, UserRole::Resident}) {
    CAPTURE(userRoleToString(role));
    const JwtContext who = contextOf(5, role);

    setGate(true, true);
    const Json::Value allOn = drogon::sync_wait(service.sync(body, who));
    CHECK(allOn["info"]["project"]["created"].size() == 1);
    CHECK(allOn["info"]["event"]["created"].isArray());

    setGate(true, false);
    const Json::Value noProductivity = drogon::sync_wait(service.sync(body, who));
    CHECK(noProductivity["info"]["project"].isNull());
    CHECK(noProductivity["info"]["calendar_event"].isNull());
    CHECK(noProductivity["info"]["reminder"]["created"].size() == 1);
    CHECK(noProductivity["info"]["reminder"]["created"][0]["userId"] == 5);
    CHECK(noProductivity["info"]["reminder_detail"]["created"].size() == 1);
    CHECK(noProductivity["info"]["event"]["created"].isArray());

    setGate(false, true);
    const Json::Value noSurveillance = drogon::sync_wait(service.sync(body, who));
    CHECK(noSurveillance["info"]["event"].isNull());
    CHECK(noSurveillance["info"]["project"]["created"].size() == 1);
    CHECK(noSurveillance["info"]["reminder"]["created"].size() == 1);
  }

  const auto tablesOn = SynchronizedService::auditTablesFor(UserRole::Resident, snapshotWith(true, true));
  const auto tablesOff = SynchronizedService::auditTablesFor(UserRole::Resident, snapshotWith(false, false));
  CHECK(std::ranges::find(tablesOn, TableName::Camera) != tablesOn.end());
  CHECK(std::ranges::find(tablesOn, TableName::Project) != tablesOn.end());
  CHECK(std::ranges::find(tablesOff, TableName::Camera) == tablesOff.end());
  CHECK(std::ranges::find(tablesOff, TableName::Project) == tablesOff.end());
  CHECK(std::ranges::find(tablesOff, TableName::Person) != tablesOff.end());
  moduleGate().reset();
}

TEST_CASE("no producer can forge a context update into the sockets")
{
  const Json::Value forged = json_util::fromString(
      R"({"operation":13,"option":"user","info":{"role":"owner"},"users":[1]})");
  CHECK_FALSE(sync_fan_out::parseEvent(forged).has_value());
  CHECK_FALSE(sync_fan_out::parseEvent(forged, sync_fan_out::ControlScope::All).has_value());
}

TEST_CASE("a role this build does not know joins no module room")
{
  setGate(true, true);
  SyncService service;
  const auto socket = connect(service, 51, UserRole::Unknown);
  const Json::Value first = json_util::fromString(socket->messages.front());
  CHECK(first["info"]["role"] == "unknown");
  CHECK(first["info"]["context"]["roleActive"] == false);
  CHECK(first["info"]["context"]["capabilities"].empty());
  CHECK_FALSE(RoomManager{}.isOnline(moduleRoom(TableName::Camera)));
  CHECK_FALSE(RoomManager{}.isOnline(moduleRoom(TableName::Notification)));
  leave(socket);
  moduleGate().reset();
}

TEST_CASE("a producer that sends no kind gets one inferred: core for core, available for the rest")
{
  ModuleFlag bare;
  bare.id = "productivity";
  ModuleFlag core;
  core.id = "core";
  const ModuleSnapshot modules({core, bare});
  const Json::Value none;
  const Json::Value context = user_context::build({.userId = 1, .role = UserRole::Guest, .modules = modules, .ownerCatalog = none, .remote = kNoRemote});
  CHECK(context["modules"][0]["kind"] == "core");
  CHECK(context["modules"][1]["kind"] == "available");
}

TEST_CASE("the context names the zone the household runs in, taken from TZ")
{
  setenv("TZ", "America/New_York", 1);
  const auto modules = snapshotWith(true, true);
  const Json::Value none;
  const Json::Value context = user_context::build({.userId = 1, .role = UserRole::Guest, .modules = modules, .ownerCatalog = none, .remote = kNoRemote});
  unsetenv("TZ");
  CHECK(context["timeZone"] == "America/New_York");
}

TEST_CASE("every role with a session gets remote in its context, and Unknown gets no field")
{
  const auto modules = snapshotWith(true, true);
  const Json::Value none;
  const std::optional<routes::RemoteLinkState> remote = routes::RemoteLinkState{
      .homeId = "0123456789abcdef0123456789abcdef",
      .domain = "argus-tunnel.lynk.page",
      .link = routes::RemoteLink::Up,
      .since = 1'790'000'000};
  for (const auto role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    const Json::Value context =
        user_context::build({.userId = 5, .role = role, .modules = modules, .ownerCatalog = none, .remote = remote});
    REQUIRE(context.isMember("remote"));
    CHECK(context["remote"]["homeId"] == "0123456789abcdef0123456789abcdef");
    CHECK(context["remote"]["domain"] == "argus-tunnel.lynk.page");
    CHECK(context["remote"]["link"] == "up");
    CHECK(context["remote"]["since"] == 1'790'000'000);
    CHECK(context["remote"].getMemberNames().size() == 4);
  }
  const Json::Value unknown = user_context::build(
      {.userId = 5, .role = UserRole::Unknown, .modules = modules, .ownerCatalog = none, .remote = remote});
  CHECK_FALSE(unknown.isMember("remote"));
  const Json::Value absent =
      user_context::build({.userId = 5, .role = UserRole::Owner, .modules = modules, .ownerCatalog = none, .remote = kNoRemote});
  CHECK_FALSE(absent.isMember("remote"));
}

TEST_CASE("InitialInfo carries the remote state the home last reported, and Unknown's carries none")
{
  setGate(true, true);
  userContext().setRemote(routes::RemoteLinkState{.homeId = "0123456789abcdef0123456789abcdef",
                                                  .domain = "argus-tunnel.lynk.page",
                                                  .link = routes::RemoteLink::Connecting,
                                                  .since = 1'790'000'100});
  SyncService service;
  const auto resident = connect(service, 71, UserRole::Resident);
  const Json::Value first = json_util::fromString(resident->messages.front());
  CHECK(first["info"]["context"]["remote"]["link"] == "connecting");
  CHECK(first["info"]["context"]["remote"]["since"] == 1'790'000'100);
  const auto unknown = connect(service, 72, UserRole::Unknown);
  const Json::Value unknownFirst = json_util::fromString(unknown->messages.front());
  CHECK_FALSE(unknownFirst["info"]["context"].isMember("remote"));
  leave(resident);
  leave(unknown);
  userContext().setRemote(std::nullopt);
  moduleGate().reset();
}

TEST_CASE("a remote change sends a context update to every role with a session, and Unknown gets no frame")
{
  setGate(true, true);
  userContext().setRemote(std::nullopt);
  SyncService service;
  const auto resident = connect(service, 81, UserRole::Resident);
  const auto unknown = connect(service, 82, UserRole::Unknown);
  resident->messages.clear();
  unknown->messages.clear();

  userContext().setRemote(routes::RemoteLinkState{.homeId = "0123456789abcdef0123456789abcdef",
                                                  .domain = "argus-tunnel.lynk.page",
                                                  .link = routes::RemoteLink::Down,
                                                  .since = 1'790'000'200});
  const auto modules = moduleGate().snapshot();
  const Json::Value catalog = json_util::fromString(R"([{"id":"surveillance","job":null}])");
  userContext().deliverRemote(resident, {.modules = modules, .ownerCatalog = catalog});
  userContext().deliverRemote(unknown, {.modules = modules, .ownerCatalog = catalog});

  REQUIRE(resident->messages.size() == 1);
  const Json::Value update = resident->last();
  CHECK(update["operation"] == 13);
  CHECK(update["option"] == "user");
  CHECK(update["info"]["remote"]["link"] == "down");
  CHECK(update["info"]["remote"]["homeId"] == "0123456789abcdef0123456789abcdef");
  CHECK_FALSE(update["info"].isMember("ownerCatalog"));
  CHECK(unknown->messages.empty());
  leave(resident);
  leave(unknown);
  userContext().setRemote(std::nullopt);
  moduleGate().reset();
}

TEST_CASE("two remote context updates in a row to an Owner both carry the owner catalog, and settings is asked once")
{
  setGate(true, true);
  userContext().setCatalogFetch({});
  SyncService service;
  const auto owner = connect(service, 91, UserRole::Owner);
  CHECK_FALSE(json_util::fromString(owner->messages.front())["info"]["context"].isMember("ownerCatalog"));
  owner->messages.clear();

  const auto asks = std::make_shared<std::atomic<int>>(0);
  userContext().setCatalogFetch([asks] {
    ++*asks;
    return std::optional<std::string>(R"([{"id":"surveillance","job":null}])");
  });
  const auto modules = moduleGate().snapshot();
  for (const auto link : {routes::RemoteLink::Up, routes::RemoteLink::Down}) {
    userContext().setRemote(routes::RemoteLinkState{.homeId = "0123456789abcdef0123456789abcdef",
                                                    .domain = "argus-tunnel.lynk.page",
                                                    .link = link,
                                                    .since = 1'790'000'300});
    const Json::Value catalog = drogon::sync_wait(userContext().cachedOwnerCatalog());
    userContext().deliverRemote(owner, {.modules = modules, .ownerCatalog = catalog});
  }

  REQUIRE(owner->messages.size() == 2);
  for (const auto& message : owner->messages) {
    const Json::Value update = json_util::fromString(message);
    CHECK(update["operation"] == 13);
    CHECK(update["info"]["ownerCatalog"][0]["id"] == "surveillance");
  }
  CHECK(json_util::fromString(owner->messages.front())["info"]["remote"]["link"] == "up");
  CHECK(json_util::fromString(owner->messages.back())["info"]["remote"]["link"] == "down");
  CHECK(asks->load() == 1);
  leave(owner);
  userContext().setCatalogFetch({});
  userContext().setRemote(std::nullopt);
  moduleGate().reset();
}

TEST_CASE("the catalog an Owner's InitialInfo reads is the one a remote update carries, without asking settings again")
{
  setGate(true, true);
  const auto asks = std::make_shared<std::atomic<int>>(0);
  const auto reply = std::make_shared<std::optional<std::string>>(R"([{"id":"surveillance","job":null}])");
  userContext().setCatalogFetch([asks, reply] {
    ++*asks;
    return *reply;
  });
  SyncService service;
  const auto first = connect(service, 92, UserRole::Owner);
  *reply = R"([{"id":"productivity","job":null}])";
  const auto second = connect(service, 93, UserRole::Owner);
  CHECK(asks->load() == 2);
  first->messages.clear();

  userContext().setRemote(routes::RemoteLinkState{.homeId = "0123456789abcdef0123456789abcdef",
                                                  .domain = "argus-tunnel.lynk.page",
                                                  .link = routes::RemoteLink::Up,
                                                  .since = 1'790'000'400});
  const Json::Value catalog = drogon::sync_wait(userContext().cachedOwnerCatalog());
  userContext().deliverRemote(first, {.modules = moduleGate().snapshot(), .ownerCatalog = catalog});
  REQUIRE(first->messages.size() == 1);
  CHECK(first->last()["info"]["ownerCatalog"][0]["id"] == "productivity");
  CHECK(asks->load() == 2);
  leave(first);
  leave(second);
  userContext().setCatalogFetch({});
  userContext().setRemote(std::nullopt);
  moduleGate().reset();
}

TEST_CASE("an ask settings does not answer leaves no catalog held: the next remote update asks once and goes without")
{
  setGate(true, true);
  const auto asks = std::make_shared<std::atomic<int>>(0);
  const auto reply = std::make_shared<std::optional<std::string>>(R"([{"id":"surveillance","job":null}])");
  userContext().setCatalogFetch([asks, reply] {
    ++*asks;
    return *reply;
  });
  SyncService service;
  const auto owner = connect(service, 94, UserRole::Owner);
  CHECK(json_util::fromString(owner->messages.front())["info"]["context"]["ownerCatalog"][0]["id"] == "surveillance");
  *reply = std::nullopt;
  const auto silent = connect(service, 95, UserRole::Owner);
  CHECK_FALSE(json_util::fromString(silent->messages.front())["info"]["context"].isMember("ownerCatalog"));
  CHECK(asks->load() == 2);
  owner->messages.clear();

  userContext().setRemote(routes::RemoteLinkState{.homeId = "0123456789abcdef0123456789abcdef",
                                                  .domain = "argus-tunnel.lynk.page",
                                                  .link = routes::RemoteLink::Connecting,
                                                  .since = 1'790'000'500});
  const Json::Value unanswered = drogon::sync_wait(userContext().cachedOwnerCatalog());
  userContext().deliverRemote(owner, {.modules = moduleGate().snapshot(), .ownerCatalog = unanswered});
  CHECK(asks->load() == 3);
  REQUIRE(owner->messages.size() == 1);
  CHECK(owner->last()["info"]["remote"]["link"] == "connecting");
  CHECK_FALSE(owner->last()["info"].isMember("ownerCatalog"));
  leave(owner);
  leave(silent);
  userContext().setCatalogFetch({});
  userContext().setRemote(std::nullopt);
  moduleGate().reset();
}

TEST_CASE("the held catalog follows the newest ask: an older answer arriving late never replaces it")
{
  const Json::Value surveillance = json_util::fromString(R"([{"id":"surveillance"}])");
  const Json::Value productivity = json_util::fromString(R"([{"id":"productivity"}])");
  user_context::CatalogHold hold;
  CHECK_FALSE(hold.held().has_value());

  const uint64_t older = hold.begin();
  const uint64_t newer = hold.begin();
  hold.keep(newer, productivity);
  hold.keep(older, surveillance);
  CHECK(hold.held() == std::optional<Json::Value>(productivity));

  hold.keep(hold.begin(), Json::Value(Json::nullValue));
  CHECK_FALSE(hold.held().has_value());

  hold.keep(hold.begin(), surveillance);
  CHECK(hold.held() == std::optional<Json::Value>(surveillance));
  hold.keep(hold.begin(), json_util::fromString(R"({"id":"core"})"));
  CHECK_FALSE(hold.held().has_value());

  const uint64_t inFlight = hold.begin();
  hold.forget();
  hold.keep(inFlight, productivity);
  CHECK_FALSE(hold.held().has_value());
  hold.keep(inFlight + 7, productivity);
  CHECK_FALSE(hold.held().has_value());
}
