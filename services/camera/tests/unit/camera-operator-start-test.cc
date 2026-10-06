#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/monitor/camera-health-monitor.hxx>
#include <feature/operator/camera-operator-service.hxx>
#include <sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace
{
constexpr const char* kStartDb = "camera-operator-start-test.db";

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

bool waitUntil(const std::function<bool()>& condition,
               std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return condition();
}

class CountingSource final : public IFrameSource
{
public:
  drogon::Task<std::optional<CameraFrame>>
  grab(const FrameGrabRequest& request) override
  {
    if (request.cameraId > 0)
      grabs.fetch_add(1, std::memory_order_acq_rel);
    co_return std::nullopt;
  }

  std::atomic<int> grabs{0};
};

bool databaseAnswers(std::chrono::milliseconds timeout)
{
  auto answered = std::make_shared<std::atomic<bool>>(false);
  DbService::client()->execSqlAsync(
      "SELECT COUNT(*) FROM camera",
      [answered](const drogon::orm::Result&) { answered->store(true); },
      [](const drogon::orm::DrogonDbException&) {});
  return waitUntil([answered] { return answered->load(); }, timeout);
}
}

TEST_CASE("starting the operator from the database thread keeps the database answering")
{
  std::remove(kStartDb);
  std::remove((std::string(kStartDb) + "-wal").c_str());
  std::remove((std::string(kStartDb) + "-shm").c_str());
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{
      .connectionNumber = 1, .filename = kStartDb, .name = "default", .timeout = -1});
  AppRunner runner;
  REQUIRE(waitUntil([] { return drogon::app().isRunning(); },
                    std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_CAMERA_SCHEMA_PATH));
  DbService::client()->execSqlSync(
      "INSERT INTO camera(name, ip) VALUES('Front', '127.0.0.2')");

  CountingSource source;
  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {.detector = nullptr,
                         .source = &source,
                         .sink = nullptr,
                         .matcher = nullptr,
                         .zones = nullptr};
  inputs.objects.maxFpsInference = 20.0;
  inputs.operator_.cameraRescanMs = 50;
  CameraOperatorService service(inputs);

  auto started = std::make_shared<std::atomic<bool>>(false);
  DbService::client()->execSqlAsync(
      "SELECT 1",
      [&service, started](const drogon::orm::Result&) {
        service.start();
        started->store(true);
      },
      [](const drogon::orm::DrogonDbException&) {});

  if (!waitUntil([started] { return started->load(); },
                 std::chrono::seconds(5))) {
    std::cerr << "CameraOperatorService::start() blocked the database thread\n";
    std::_Exit(EXIT_FAILURE);
  }
  CHECK(databaseAnswers(std::chrono::seconds(5)));
  CHECK(waitUntil([&source] { return source.grabs.load() > 0; },
                  std::chrono::seconds(5)));
  CHECK(databaseAnswers(std::chrono::seconds(5)));

  service.requestStop();
  CHECK(waitUntil([&service] { return service.drained(); },
                  std::chrono::seconds(5)));

  INFO("the operator idles its camera loops while surveillance is disabled and resumes on enable");
  {
    auto active = std::make_shared<std::atomic<bool>>(true);
    CountingSource gated;
    CameraOperatorService::Inputs gatedInputs;
    gatedInputs.dependencies = {.detector = nullptr,
                                .source = &gated,
                                .sink = nullptr,
                                .matcher = nullptr,
                                .zones = nullptr,
                                .active = [active] { return active->load(); }};
    gatedInputs.objects.maxFpsInference = 20.0;
    gatedInputs.operator_.cameraRescanMs = 50;
    CameraOperatorService gatedService(gatedInputs);
    drogon::app().getLoop()->queueInLoop([&gatedService] { gatedService.start(); });
    CHECK(waitUntil([&gatedService] { return gatedService.cameraLoops() == 1; },
                    std::chrono::seconds(5)));

    active->store(false);
    CHECK(waitUntil([&gatedService] { return gatedService.cameraLoops() == 0; },
                    std::chrono::seconds(5)));

    const int grabsWhileIdle = gated.grabs.load();
    active->store(true);
    CHECK(waitUntil([&gatedService] { return gatedService.cameraLoops() == 1; },
                    std::chrono::seconds(5)));
    CHECK(waitUntil([&gated, grabsWhileIdle] { return gated.grabs.load() > grabsWhileIdle; },
                    std::chrono::seconds(5)));
    gatedService.requestStop();
    CHECK(waitUntil([&gatedService] { return gatedService.drained(); },
                    std::chrono::seconds(5)));
  }

  INFO("the health monitor idles while surveillance is disabled and samples again on enable");
  {
    auto active = std::make_shared<std::atomic<bool>>(false);
    CountingSource sampled;
    CameraHealthMonitor monitor({.source = &sampled,
                                 .sink = nullptr,
                                 .presence = nullptr,
                                 .active = [active] { return active->load(); }},
                                {.enabled = true,
                                 .intervalMs = 50,
                                 .thresholds = {},
                                 .rebaselineAfterMs = 900000});
    drogon::app().getLoop()->queueInLoop([&monitor] { monitor.start(); });
    CHECK(waitUntil([&monitor] { return monitor.idle(); }, std::chrono::seconds(5)));
    CHECK(sampled.grabs.load() == 0);

    active->store(true);
    CHECK(waitUntil([&sampled] { return sampled.grabs.load() > 0; }, std::chrono::seconds(5)));
    CHECK_FALSE(monitor.idle());
    monitor.requestStop();
    CHECK(waitUntil([&monitor] { return monitor.drained(); }, std::chrono::seconds(5)));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
}
