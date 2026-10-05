#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
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
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
}
