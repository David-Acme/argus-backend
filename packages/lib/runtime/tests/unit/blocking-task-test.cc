#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <runtime/blocking-task.hxx>

#include "app-runner.hxx"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{

void onLoop(std::function<drogon::Task<void>()> body)
{
  drogon::app().getLoop()->queueInLoop(
      [body = std::move(body)]() { drogon::async_run(body); });
}

struct Outcome
{
  std::atomic<bool> done{false};
  std::atomic<bool> resumedOnLoop{false};
  std::mutex mutex;
  std::string value;
};

}

static void valueResumesOnLoop()
{
  auto outcome = std::make_shared<Outcome>();
  onLoop([outcome]() -> drogon::Task<void> {
    const auto value = co_await BlockingTask<std::string>([] {
      std::this_thread::sleep_for(5ms);
      return std::string("ready");
    });
    {
      std::scoped_lock lock(outcome->mutex);
      outcome->value = value;
    }
    outcome->resumedOnLoop = drogon::app().getLoop()->isInLoopThread();
    outcome->done = true;
  });

  REQUIRE(waitUntil([outcome] { return outcome->done.load(); }, 5s));
  CHECK(outcome->resumedOnLoop.load());
  std::scoped_lock lock(outcome->mutex);
  CHECK(outcome->value == "ready");
}

static void carriesNonDefaultConstructible()
{
  struct Only
  {
    explicit Only(int v) : value(v) {}
    int value;
  };
  auto seen = std::make_shared<std::atomic<int>>(0);
  onLoop([seen]() -> drogon::Task<void> {
    const auto only =
        co_await BlockingTask<Only>([] { return Only(42); }, BlockingLane::Heavy);
    seen->store(only.value);
  });

  CHECK(waitUntil([seen] { return seen->load() == 42; }, 5s));
}

static void rethrowsOffLoopException()
{
  auto caught = std::make_shared<std::atomic<bool>>(false);
  onLoop([caught]() -> drogon::Task<void> {
    try {
      co_await BlockingTask<void>([] { throw std::runtime_error("off loop"); });
    }
    catch (const std::runtime_error& error) {
      caught->store(std::string(error.what()) == "off loop");
    }
  });

  CHECK(waitUntil([caught] { return caught->load(); }, 5s));
}

static void strandKeepsArrivalOrder()
{
  static BlockingStrand strand;
  auto order = std::make_shared<std::vector<int>>();
  auto mutex = std::make_shared<std::mutex>();
  auto finished = std::make_shared<std::atomic<int>>(0);
  onLoop([order, mutex, finished]() -> drogon::Task<void> {
    for (int i = 0; i < 50; ++i) {
      drogon::async_run([i, order, mutex, finished]() -> drogon::Task<void> {
        co_await BlockingTask<void>(
            [i, order, mutex] {
              std::this_thread::sleep_for(std::chrono::microseconds((50 - i) * 20));
              std::scoped_lock lock(*mutex);
              order->push_back(i);
            },
            strand);
        finished->fetch_add(1);
      });
    }
    co_return;
  });

  REQUIRE(waitUntil([finished] { return finished->load() == 50; }, 10s));
  std::scoped_lock lock(*mutex);
  REQUIRE(order->size() == 50);
  for (int i = 0; i < 50; ++i)
    CHECK((*order)[static_cast<std::size_t>(i)] == i);
}

static void thousandAwaitsStayCapped()
{
  auto finished = std::make_shared<std::atomic<int>>(0);
  onLoop([finished]() -> drogon::Task<void> {
    for (int i = 0; i < 1000; ++i) {
      drogon::async_run([finished]() -> drogon::Task<void> {
        co_await BlockingTask<int>([] {
          std::this_thread::sleep_for(2ms);
          return 1;
        });
        finished->fetch_add(1);
      });
    }
    co_return;
  });

  REQUIRE(waitUntil([finished] { return finished->load() == 1000; }, 30s));
  const auto stats = blocking_pool::statsOf(BlockingLane::Light);
  CHECK(stats.peakThreads <= blocking_pool::limitsFor(BlockingLane::Light).maxThreads);
  CHECK(stats.peakThreads >= 2);
}

static void ioLoopAwaitStaysOnItsLoop()
{
  auto* io = drogon::app().getIOLoop(0);
  REQUIRE(io != nullptr);
  auto resumedOnIo = std::make_shared<std::atomic<int>>(0);
  io->queueInLoop([io, resumedOnIo]() {
    drogon::async_run([io, resumedOnIo]() -> drogon::Task<void> {
      co_await BlockingTask<void>([] { std::this_thread::sleep_for(2ms); });
      resumedOnIo->store(io->isInLoopThread() ? 1 : 2);
    });
  });

  REQUIRE(waitUntil([resumedOnIo] { return resumedOnIo->load() != 0; }, 5s));
  CHECK(resumedOnIo->load() == 1);
}

static void foreignThreadAwaitFallsBackToMainLoop()
{
  auto resumedOnMain = std::make_shared<std::atomic<int>>(0);
  std::thread foreign([resumedOnMain]() {
    drogon::async_run([resumedOnMain]() -> drogon::Task<void> {
      co_await BlockingTask<void>([] {});
      resumedOnMain->store(drogon::app().getLoop()->isInLoopThread() ? 1 : 2);
    });
  });
  foreign.join();

  REQUIRE(waitUntil([resumedOnMain] { return resumedOnMain->load() != 0; }, 5s));
  CHECK(resumedOnMain->load() == 1);
}

static void boundedAdmissionThrowsIntoTheCoroutine()
{
  ElasticPool pool({.coreThreads = 1, .maxThreads = 1,
                    .keepAlive = std::chrono::seconds(5), .maxQueued = 1});
  BlockingStrand strand(pool, 1);
  auto release = std::make_shared<std::atomic<bool>>(false);
  strand.post([release] {
    while (!release->load())
      std::this_thread::sleep_for(1ms);
  });
  strand.post([] {});
  auto refused = std::make_shared<std::atomic<int>>(0);
  onLoop([&strand, refused]() -> drogon::Task<void> {
    try {
      co_await BlockingTask<void>([] {}, strand, BlockingAdmission::RejectWhenFull);
      refused->store(2);
    }
    catch (const BlockingLaneFull&) {
      refused->store(1);
    }
  });

  REQUIRE(waitUntil([refused] { return refused->load() != 0; }, 5s));
  CHECK(refused->load() == 1);
  release->store(true);
  CHECK(waitUntil([&strand] { return strand.queued() == 0; }, 5s));
}

TEST_CASE("a blocking task awaited before the app runs resumes inline")
{
  REQUIRE_FALSE(drogon::app().isRunning());
  const int value = drogon::sync_wait([]() -> drogon::Task<int> {
    co_return co_await BlockingTask<int>([] { return 7; });
  }());
  CHECK(value == 7);
}

TEST_CASE("blocking tasks resume their coroutines on the main loop")
{
  AppRunner runner;
  REQUIRE(waitForBoot(5s));
  INFO("a blocking task hands its value back on the main loop");
  valueResumesOnLoop();
  INFO("a value type without a default constructor is carried");
  carriesNonDefaultConstructible();
  INFO("an exception thrown off the loop is rethrown in the coroutine");
  rethrowsOffLoopException();
  INFO("a strand keeps the order in which the coroutines reached it");
  strandKeepsArrivalOrder();
  INFO("a thousand concurrent awaits stay inside the light lane's cap");
  thousandAwaitsStayCapped();
  INFO("an await started on an IO loop resumes on that IO loop");
  ioLoopAwaitStaysOnItsLoop();
  INFO("an await started off every app loop resumes on the main loop");
  foreignThreadAwaitFallsBackToMainLoop();
  INFO("a refused admission is thrown into the awaiting coroutine");
  boundedAdmissionThrowsIntoTheCoroutine();
}
