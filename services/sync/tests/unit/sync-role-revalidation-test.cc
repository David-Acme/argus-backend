#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <auth/jwt-filter.hxx>
#include <auth/module-gate.hxx>
#include <auth/user-directory.hxx>
#include <auth/user-role.hxx>
#include <drogon/drogon.h>
#include <feature/transport/services/connection-lanes.hxx>
#include <feature/transport/services/frame-lane.hxx>
#include <shared/services/context/user-context.hxx>
#include <shared/services/room/room-manager.hxx>
#include <sync/sync-operation.hxx>
#include <text/json-util.hxx>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
constexpr auto kLoopWait = std::chrono::seconds{10};
constexpr auto kPollStep = std::chrono::milliseconds{20};
constexpr int64_t kMovedUser = 7;
constexpr int64_t kSteadyUser = 8;

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

bool runOnLoop(std::size_t index, const std::function<void()>& action)
{
  auto* loop = drogon::app().getIOLoop(index);
  if (loop == nullptr)
    return false;
  auto done = std::make_shared<std::promise<void>>();
  auto finished = done->get_future();
  loop->runInLoop([&action, done]() {
    action();
    done->set_value();
  });
  return finished.wait_for(kLoopWait) == std::future_status::ready;
}

bool settlesOnLoop(std::size_t index, const std::function<bool()>& settled)
{
  const auto deadline = std::chrono::steady_clock::now() + kLoopWait;
  for (;;) {
    bool answered = false;
    if (!runOnLoop(index, [&settled, &answered]() { answered = settled(); }))
      return false;
    if (answered)
      return true;
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(kPollStep);
  }
}

bool holdsRoom(RoomId room, const drogon::WebSocketConnection* conn)
{
  bool found = false;
  RoomManager::visitLocalMembers(room, [&found, conn](const Conn& member) {
    found = found || member.get() == conn;
  });
  return found;
}

int contextUpdates(const std::vector<std::string>& messages, std::string& role)
{
  int count = 0;
  role.clear();
  for (const auto& message : messages) {
    const Json::Value frame = json_util::fromString(message);
    if (frame["operation"].asInt() !=
        static_cast<int>(SyncOperation::ContextUpdate))
      continue;
    ++count;
    role = frame["info"]["role"].asString();
  }
  return count;
}

std::vector<RoomId> roomsOf(UserRole role, const ModuleSnapshot& modules,
                            int64_t userId)
{
  std::vector<RoomId> rooms = roleRoomsOf(role, modules);
  rooms.push_back(userRoom(userId));
  rooms.push_back(kConnectedRoom);
  return rooms;
}

class RecordingConnection final : public drogon::WebSocketConnection
{
public:
  void send(const char* msg, uint64_t len,
            const drogon::WebSocketMessageType) override
  {
    messages.emplace_back(msg, len);
  }
  void send(std::string_view msg, const drogon::WebSocketMessageType) override
  {
    messages.emplace_back(msg);
  }
  void sendJson(const Json::Value& json,
                const drogon::WebSocketMessageType) override
  {
    messages.push_back(json_util::toString(json));
  }
  [[nodiscard]] const trantor::InetAddress& localAddr() const override
  {
    return addr_;
  }
  [[nodiscard]] const trantor::InetAddress& peerAddr() const override
  {
    return addr_;
  }
  [[nodiscard]] bool connected() const override { return closeReason.empty(); }
  [[nodiscard]] bool disconnected() const override
  {
    return !closeReason.empty();
  }
  void shutdown(const drogon::CloseCode, const std::string& reason) override
  {
    closeReason = reason;
  }
  void forceClose() override { closeReason = "forced"; }
  void setPingMessage(const std::string&,
                      const std::chrono::duration<double>&) override
  {
  }
  void disablePing() override {}

  std::vector<std::string> messages;
  std::string closeReason;

private:
  trantor::InetAddress addr_{"127.0.0.1", 0};
};

class RoleDirectory final : public IUserDirectory
{
public:
  drogon::Task<DirectoryLookup> lookup(int64_t userId) const override
  {
    ++calls;
    const UserRole answer = calls == 1 ? firstLookup : laterLookup;
    co_return DirectoryLookup{
        .status = DirectoryLookupStatus::Found,
        .user = DirectoryUser{.id = userId,
                              .name = "Ana",
                              .lastName = "Garcia",
                              .lang = "es",
                              .role = answer,
                              .isActive = true}};
  }

  mutable int calls{0};
  UserRole firstLookup{UserRole::Guest};
  UserRole laterLookup{UserRole::Guest};
};

struct Rig
{
  Rig()
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().setThreadNum(1);
    moduleGate().apply({});
    runner.emplace();
    if (!waitForBoot(std::chrono::seconds(30)))
      throw std::runtime_error("drogon loop did not boot");
  }

  std::optional<AppRunner> runner;
};

std::shared_ptr<RecordingConnection> socketFor(int64_t userId,
                                               const std::string& name,
                                               UserRole role)
{
  const auto conn = std::make_shared<RecordingConnection>();
  conn->setContext(std::make_shared<JwtContext>(JwtContext{.sub = userId,
                                                          .name = name,
                                                          .role = role,
                                                          .isActive = true,
                                                          .deviceHash = {},
                                                          .sessionId = {}}));
  return conn;
}

struct LanesFixture
{
  explicit LanesFixture(const std::shared_ptr<IUserDirectory>& directory)
  {
    lanes.setDrainStarter(
        [this](const drogon::WebSocketConnectionPtr&,
               const std::shared_ptr<FrameLane>& lane) {
          started.push_back(lane->userId());
        });
    lanes.startRevalidation({.intervalSeconds = 3600, .directory = directory});
  }

  ConnectionLanes lanes;
  std::vector<int64_t> started;
};
}

Rig* g_rig = nullptr;

Rig& sharedRig()
{
  return *g_rig;
}

TEST_CASE("a role the revalidation finds moved takes the socket and re-sends its context")
{
  sharedRig();
  const ModuleSnapshot modules = moduleGate().snapshot();
  const auto conn = socketFor(kMovedUser, "Ana Garcia", UserRole::Resident);
  const drogon::WebSocketConnectionPtr handle = conn;

  LanesFixture fixture(std::make_shared<RoleDirectory>());
  fixture.lanes.open({.conn = handle, .userId = kMovedUser,
                      .loop = drogon::app().getIOLoop(0)});

  REQUIRE(runOnLoop(0, [&modules, &handle]() {
    RoomManager{}.joinMany(roomsOf(UserRole::Resident, modules, kMovedUser),
                           handle);
  }));
  userContext().remember(kMovedUser, UserRole::Resident);

  drogon::sync_wait(fixture.lanes.revalidateAll());

  CHECK(userContext().roleOf(kMovedUser).value_or(UserRole::Unknown) ==
        UserRole::Guest);
  CHECK(fixture.started == std::vector<int64_t>{kMovedUser});

  bool leftOldRoom = false;
  bool joinedNewRoom = false;
  bool socketRole = false;
  int frames = -1;
  std::string frameRole;

  REQUIRE(settlesOnLoop(0, [&]() {
    leftOldRoom = !holdsRoom(roleRoom(UserRole::Resident), conn.get());
    joinedNewRoom = holdsRoom(roleRoom(UserRole::Guest), conn.get());
    socketRole = conn->getContextRef<JwtContext>().role == UserRole::Guest;
    frames = contextUpdates(conn->messages, frameRole);
    return leftOldRoom && joinedNewRoom && socketRole && frames == 1 &&
           frameRole == userRoleToString(UserRole::Guest);
  }));

  CHECK(leftOldRoom);
  CHECK(joinedNewRoom);
  CHECK(socketRole);
  CHECK(frames == 1);
  CHECK(frameRole == userRoleToString(UserRole::Guest));

  const auto lane = fixture.lanes.find(handle);
  REQUIRE(lane);
  const auto verdict = lane->next();
  if (!verdict) {
    FAIL("expected the lane to hold the revalidation");
    return;
  }
  CHECK(verdict->kind == FrameJobKind::Revalidate);
  CHECK_FALSE(lane->next().has_value());

  drogon::sync_wait(fixture.lanes.revalidateAll());

  CHECK(fixture.started == std::vector<int64_t>{kMovedUser, kMovedUser});
  CHECK(settlesOnLoop(0, [&]() {
    std::string laterRole;
    return contextUpdates(conn->messages, laterRole) == 1 &&
           laterRole == userRoleToString(UserRole::Guest) &&
           !holdsRoom(roleRoom(UserRole::Resident), conn.get()) &&
           holdsRoom(roleRoom(UserRole::Guest), conn.get());
  }));

  userContext().remember(kMovedUser, UserRole::Unknown);
  REQUIRE(runOnLoop(0, [&handle]() { RoomManager{}.leaveAll(handle); }));
}

TEST_CASE("a role the revalidation finds unchanged moves nothing and sends nothing")
{
  sharedRig();
  const ModuleSnapshot modules = moduleGate().snapshot();
  const auto conn = socketFor(kSteadyUser, "Luis Diaz", UserRole::Resident);
  const drogon::WebSocketConnectionPtr handle = conn;

  const auto directory = std::make_shared<RoleDirectory>();
  directory->firstLookup = UserRole::Resident;
  directory->laterLookup = UserRole::Resident;
  LanesFixture fixture(directory);
  fixture.lanes.open({.conn = handle, .userId = kSteadyUser,
                      .loop = drogon::app().getIOLoop(0)});

  REQUIRE(runOnLoop(0, [&modules, &handle]() {
    RoomManager{}.joinMany(roomsOf(UserRole::Resident, modules, kSteadyUser),
                           handle);
  }));
  userContext().remember(kSteadyUser, UserRole::Resident);

  drogon::sync_wait(fixture.lanes.revalidateAll());

  CHECK(userContext().roleOf(kSteadyUser).value_or(UserRole::Unknown) ==
        UserRole::Resident);
  CHECK(fixture.started == std::vector<int64_t>{kSteadyUser});
  CHECK(directory->calls == 1);

  bool holdsOldRoom = false;
  bool holdsNewRoom = false;
  bool socketRole = false;
  int frames = -1;
  std::string frameRole;

  REQUIRE(settlesOnLoop(0, [&]() {
    holdsOldRoom = holdsRoom(roleRoom(UserRole::Resident), conn.get());
    holdsNewRoom = holdsRoom(roleRoom(UserRole::Guest), conn.get());
    socketRole = conn->getContextRef<JwtContext>().role == UserRole::Resident;
    frames = contextUpdates(conn->messages, frameRole);
    return holdsOldRoom && !holdsNewRoom && socketRole && frames == 0;
  }));

  CHECK(holdsOldRoom);
  CHECK_FALSE(holdsNewRoom);
  CHECK(socketRole);
  CHECK(frames == 0);

  userContext().remember(kSteadyUser, UserRole::Unknown);
  REQUIRE(runOnLoop(0, [&handle]() { RoomManager{}.leaveAll(handle); }));
}

int main(int argc, char** argv)
{
  try {
    Rig rig;
    g_rig = &rig;
    doctest::Context context(argc, argv);
    const int status = context.run();
    g_rig = nullptr;
    return status;
  }
  catch (const std::exception& error) {
    LOG_FATAL << "the fixture did not start: " << error.what();
    return 1;
  }
}
