#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <auth/jwt-filter.hxx>
#include <chrono>
#include <cstdio>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/talk/camera-talk-service.hxx>
#include <memory>
#include <mutex>
#include <shared/services/camera-driver/camera-driver.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_CAMERA_SCHEMA_PATH
#error "ARGUS_CAMERA_SCHEMA_PATH must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kDb = "camera-talk-module-test.db";
constexpr int64_t kCamera = 1;

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

struct LineLog
{
  std::mutex mutex;
  int opened{0};
  int closed{0};
};

class RecordingLine final : public ICameraTalkLine
{
public:
  explicit RecordingLine(std::shared_ptr<LineLog> log) : log_(std::move(log)) {}

  DriverResult open() override
  {
    const std::scoped_lock lock(log_->mutex);
    ++log_->opened;
    return {.ok = true, .error = {}, .data = Json::Value()};
  }

  DriverResult write(std::span<const int16_t>) override { return {.ok = true, .error = {}, .data = Json::Value()}; }

  void close() override
  {
    const std::scoped_lock lock(log_->mutex);
    ++log_->closed;
  }

private:
  std::shared_ptr<LineLog> log_;
};

class TalkingDriver final : public ICameraDriver
{
public:
  explicit TalkingDriver(std::shared_ptr<LineLog> log) : log_(std::move(log)) {}

  [[nodiscard]] Json::Value capabilities() const override
  {
    Json::Value caps(Json::objectValue);
    caps["talk"] = true;
    return caps;
  }
  DriverResult status() override { return DriverResult::failed("noop"); }
  DriverResult presets() override { return DriverResult::failed("noop"); }
  DriverResult move(const DriverMoveInput&) override { return DriverResult::failed("noop"); }
  DriverResult preset(const DriverPresetInput&) override { return DriverResult::failed("noop"); }
  DriverResult settings(const DriverSettingsInput&) override { return DriverResult::failed("noop"); }
  DriverResult speak(const DriverSpeakInput&) override { return DriverResult::failed("noop"); }

  TalkLineOpen talkLine() override { return {.line = std::make_unique<RecordingLine>(log_), .error = {}}; }

private:
  std::shared_ptr<LineLog> log_;
};

class TestConnection final : public drogon::WebSocketConnection
{
public:
  void send(const char*, uint64_t, const drogon::WebSocketMessageType) override {}
  void send(std::string_view, const drogon::WebSocketMessageType) override {}
  void sendJson(const Json::Value& message, const drogon::WebSocketMessageType) override
  {
    const std::scoped_lock lock(mutex);
    frames.push_back(message);
  }
  [[nodiscard]] const trantor::InetAddress& localAddr() const override { return addr_; }
  [[nodiscard]] const trantor::InetAddress& peerAddr() const override { return addr_; }
  [[nodiscard]] bool connected() const override { return true; }
  [[nodiscard]] bool disconnected() const override { return false; }
  void shutdown(const drogon::CloseCode, const std::string&) override {}
  void forceClose() override {}
  void setPingMessage(const std::string&, const std::chrono::duration<double>&) override {}
  void disablePing() override {}

  [[nodiscard]] std::string lastClosedReason() const
  {
    const std::scoped_lock lock(mutex);
    for (auto frame = frames.rbegin(); frame != frames.rend(); ++frame)
      if ((*frame)["type"].asString() == "camera:talk:closed")
        return (*frame)["payload"]["reason"].asString();
    return {};
  }

  [[nodiscard]] bool announced() const
  {
    const std::scoped_lock lock(mutex);
    for (const auto& frame : frames)
      if (frame["type"].asString() == "camera:talk:ready")
        return true;
    return false;
  }

  mutable std::mutex mutex;
  std::vector<Json::Value> frames;

private:
  trantor::InetAddress addr_{"127.0.0.1", 0};
};

drogon::WebSocketConnectionPtr connectionOf(int64_t userId, UserRole role)
{
  auto connection = std::make_shared<TestConnection>();
  connection->setContext(std::make_shared<JwtContext>(JwtContext{.sub = userId,
                                                                 .name = "Ana",
                                                                 .role = role,
                                                                 .isActive = true,
                                                                 .deviceHash = {},
                                                                 .sessionId = {}}));
  return connection;
}

struct StartFrame
{
  explicit StartFrame(drogon::WebSocketConnectionPtr connection) : conn(std::move(connection))
  {
    message["type"] = "camera:talk:start";
    message["payload"]["cameraId"] = static_cast<Json::Int64>(kCamera);
    message["payload"]["sampleRate"] = 16000;
  }

  [[nodiscard]] SyncFrameInput input() const { return {.conn = conn, .message = message, .raw = {}}; }

  drogon::WebSocketConnectionPtr conn;
  Json::Value message{Json::objectValue};
};

bool waitUntil(const std::function<bool()>& done)
{
  for (int i = 0; i < 400 && !done(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return done();
}
}

TEST_CASE("a talk session ends when surveillance is turned off, and none starts while it is off")
{
  std::remove(kDb);
  std::remove((std::string(kDb) + "-wal").c_str());
  std::remove((std::string(kDb) + "-shm").c_str());
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{
      .connectionNumber = 1, .filename = kDb, .name = "default", .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_CAMERA_SCHEMA_PATH));
  DbService::cameraClient()->execSqlSync(
      "INSERT INTO camera (id, name, ip, password) VALUES (1, 'Patio', '10.0.0.5', 'secret')");

  const auto log = std::make_shared<LineLog>();
  CameraDriverTestAccess::install(kCamera, std::make_shared<TalkingDriver>(log));

  const auto active = std::make_shared<std::atomic<bool>>(true);
  CameraTalkService service({}, [active] { return active->load(); });

  const auto ana = connectionOf(7, UserRole::Resident);
  CHECK(drogon::sync_wait(service.handleText(StartFrame(ana).input())));
  const auto announced = std::dynamic_pointer_cast<TestConnection>(ana);
  REQUIRE(announced != nullptr);
  CHECK(waitUntil([&] { return announced->announced(); }));
  CHECK(service.active() == 1);

  CHECK(service.stopAll("module_disabled") == 1);
  CHECK(waitUntil([&] { return service.active() == 0; }));
  CHECK(announced->lastClosedReason() == "module_disabled");
  {
    const std::scoped_lock lock(log->mutex);
    CHECK(log->opened == 1);
    CHECK(log->closed == 1);
  }
  CHECK(service.stopAll("module_disabled") == 0);

  active->store(false);
  const auto luis = connectionOf(8, UserRole::Guard);
  bool refused = false;
  try {
    drogon::sync_wait(service.handleText(StartFrame(luis).input()));
  }
  catch (const ResponseException& error) {
    refused = error.statusCode() == 403 && error.errorCode() == "MODULE_DISABLED";
  }
  CHECK(refused);
  CHECK(service.active() == 0);

  active->store(true);
  CHECK(drogon::sync_wait(service.handleText(StartFrame(luis).input())));
  CHECK(service.active() == 1);
  CHECK(service.stopAll("done") == 1);
  CHECK(waitUntil([&] { return service.active() == 0; }));
}
