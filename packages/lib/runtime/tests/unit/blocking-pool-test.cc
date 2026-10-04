#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <runtime/blocking-pool.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{

bool waitUntil(const std::function<bool()>& ready, std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready())
      return true;
    std::this_thread::sleep_for(2ms);
  }
  return ready();
}

class Gate
{
public:
  void open()
  {
    {
      std::scoped_lock lock(mutex_);
      open_ = true;
    }
    ready_.notify_all();
  }

  void pass()
  {
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] { return open_; });
  }

private:
  std::mutex mutex_;
  std::condition_variable ready_;
  bool open_{false};
};

}

TEST_CASE("every submitted job runs once")
{
  ElasticPool pool({.coreThreads = 2, .maxThreads = 4, .keepAlive = 1s});
  std::atomic<int> runs{0};
  for (int i = 0; i < 500; ++i)
    pool.submit([&runs] { runs.fetch_add(1); });

  REQUIRE(waitUntil([&runs] { return runs.load() == 500; }, 5s));
  REQUIRE(waitUntil([&pool] { return pool.stats().completed == 500; }, 5s));
  CHECK(pool.stats().queued == 0);
}

TEST_CASE("a burst never starts more workers than the lane allows")
{
  ElasticPool pool({.coreThreads = 1, .maxThreads = 6, .keepAlive = 1s});
  std::atomic<int> running{0};
  std::atomic<int> peak{0};
  std::atomic<int> done{0};
  for (int i = 0; i < 300; ++i) {
    pool.submit([&] {
      const int now = running.fetch_add(1) + 1;
      int seen = peak.load();
      while (now > seen && !peak.compare_exchange_weak(seen, now)) {
      }
      std::this_thread::sleep_for(1ms);
      running.fetch_sub(1);
      done.fetch_add(1);
    });
  }

  REQUIRE(waitUntil([&done] { return done.load() == 300; }, 10s));
  CHECK(peak.load() <= 6);
  CHECK(pool.stats().peakThreads <= 6);
  CHECK(pool.stats().peakThreads >= 2);
}

TEST_CASE("jobs that wait on each other all run at once up to the cap")
{
  ElasticPool pool({.coreThreads = 0, .maxThreads = 8, .keepAlive = 1s});
  std::atomic<int> arrived{0};
  Gate gate;
  std::atomic<int> left{0};
  for (int i = 0; i < 8; ++i) {
    pool.submit([&] {
      if (arrived.fetch_add(1) + 1 == 8)
        gate.open();
      gate.pass();
      left.fetch_add(1);
    });
  }

  CHECK(waitUntil([&left] { return left.load() == 8; }, 5s));
}

TEST_CASE("workers above the core retire after the keep-alive")
{
  ElasticPool pool({.coreThreads = 1, .maxThreads = 5, .keepAlive = 40ms});
  Gate gate;
  std::atomic<int> started{0};
  for (int i = 0; i < 5; ++i) {
    pool.submit([&] {
      started.fetch_add(1);
      gate.pass();
    });
  }
  REQUIRE(waitUntil([&started] { return started.load() == 5; }, 5s));
  CHECK(pool.stats().threads == 5);
  gate.open();

  CHECK(waitUntil([&pool] { return pool.stats().threads == 1; }, 5s));
}

TEST_CASE("an idle worker is reused instead of starting another")
{
  ElasticPool pool({.coreThreads = 1, .maxThreads = 8, .keepAlive = 5s});
  for (int i = 0; i < 50; ++i) {
    std::atomic<bool> ran{false};
    pool.submit([&ran] { ran.store(true); });
    REQUIRE(waitUntil([&ran] { return ran.load(); }, 5s));
    REQUIRE(waitUntil([&pool] { return pool.stats().idle == pool.stats().threads; }, 5s));
  }
  CHECK(pool.stats().peakThreads == 1);
}

TEST_CASE("a job that throws does not take its worker down")
{
  ElasticPool pool({.coreThreads = 1, .maxThreads = 1, .keepAlive = 5s});
  pool.submit([] { throw std::runtime_error("boom"); });
  std::atomic<bool> after{false};
  pool.submit([&after] { after.store(true); });

  CHECK(waitUntil([&after] { return after.load(); }, 5s));
  CHECK(pool.stats().threads == 1);
}

TEST_CASE("a strand runs its jobs one at a time in the order they were posted")
{
  ElasticPool pool({.coreThreads = 4, .maxThreads = 8, .keepAlive = 1s});
  BlockingStrand strand(pool);
  std::mutex mutex;
  std::vector<int> order;
  std::atomic<int> inside{0};
  std::atomic<int> overlap{0};
  for (int i = 0; i < 200; ++i) {
    strand.post([&, i] {
      if (inside.fetch_add(1) != 0)
        overlap.fetch_add(1);
      {
        std::scoped_lock lock(mutex);
        order.push_back(i);
      }
      inside.fetch_sub(1);
    });
  }

  REQUIRE(waitUntil(
      [&] {
        std::scoped_lock lock(mutex);
        return order.size() == 200;
      },
      5s));
  CHECK(overlap.load() == 0);
  for (int i = 0; i < 200; ++i)
    CHECK(order[static_cast<std::size_t>(i)] == i);
}

TEST_CASE("a strand never holds a worker while it waits for its turn")
{
  ElasticPool pool({.coreThreads = 1, .maxThreads = 1, .keepAlive = 1s});
  BlockingStrand strand(pool);
  std::atomic<int> runs{0};
  std::atomic<bool> other{false};
  for (int i = 0; i < 20; ++i)
    strand.post([&runs] { runs.fetch_add(1); });
  pool.submit([&other] { other.store(true); });

  CHECK(waitUntil([&] { return runs.load() == 20 && other.load(); }, 5s));
}

TEST_CASE("the process lanes are sized from the thread budget")
{
  const auto light = blocking_pool::limitsFor(BlockingLane::Light);
  const auto heavy = blocking_pool::limitsFor(BlockingLane::Heavy);
  CHECK(light.coreThreads >= 2);
  CHECK(light.maxThreads >= 16);
  CHECK(light.maxThreads >= light.coreThreads);
  CHECK(heavy.maxThreads >= 4);
  CHECK(heavy.coreThreads == 1);
  CHECK(&blocking_pool::lane(BlockingLane::Light) !=
        &blocking_pool::lane(BlockingLane::Heavy));
}
