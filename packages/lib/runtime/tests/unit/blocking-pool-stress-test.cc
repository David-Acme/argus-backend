#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <runtime/blocking-pool.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace
{

using Submit = std::function<void(std::function<void()>)>;

struct Lanes
{
  Submit heavy;
  Submit light;
};

struct Latencies
{
  double p50Ms{0};
  double p99Ms{0};
  double maxMs{0};
};

struct ThreadCounter
{
  std::atomic<int> live{0};
  std::atomic<int> peak{0};

  void enter()
  {
    const int now = live.fetch_add(1) + 1;
    int seen = peak.load();
    while (now > seen && !peak.compare_exchange_weak(seen, now)) {
    }
  }

  void leave() { live.fetch_sub(1); }
};

bool waitFor(const std::atomic<int>& counter, int target, std::chrono::seconds timeout)
{
  const auto deadline = Clock::now() + timeout;
  while (counter.load() < target && Clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  return counter.load() >= target;
}

Latencies summarize(std::vector<double> samples)
{
  std::ranges::sort(samples);
  const auto at = [&samples](double q) {
    const auto index = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
    return samples[index];
  };
  return {.p50Ms = at(0.50), .p99Ms = at(0.99), .maxMs = samples.back()};
}

constexpr int kLongJobs = 32;
constexpr auto kLongJob = 300ms;
constexpr int kShortJobs = 200;
constexpr auto kShortJob = 1ms;
constexpr auto kShortGap = 2ms;

Latencies shortJobsUnderLoad(const Lanes& lanes)
{
  std::atomic<int> longDone{0};
  for (int i = 0; i < kLongJobs; ++i) {
    lanes.heavy([&longDone] {
      std::this_thread::sleep_for(kLongJob);
      longDone.fetch_add(1);
    });
  }
  std::this_thread::sleep_for(5ms);

  std::mutex mutex;
  std::vector<double> waits;
  waits.reserve(kShortJobs);
  std::atomic<int> shortDone{0};
  for (int i = 0; i < kShortJobs; ++i) {
    const auto submitted = Clock::now();
    lanes.light([&, submitted] {
      const double waited =
          std::chrono::duration<double, std::milli>(Clock::now() - submitted).count();
      std::this_thread::sleep_for(kShortJob);
      {
        std::scoped_lock lock(mutex);
        waits.push_back(waited);
      }
      shortDone.fetch_add(1);
    });
    std::this_thread::sleep_for(kShortGap);
  }
  REQUIRE(waitFor(shortDone, kShortJobs, 60s));
  REQUIRE(waitFor(longDone, kLongJobs, 60s));
  std::scoped_lock lock(mutex);
  return summarize(waits);
}

void report(const char* name, const Latencies& latencies, int peakThreads)
{
  std::printf("%-28s short-job wait p50 %7.2f ms  p99 %8.2f ms  max %8.2f ms  peak threads %d\n",
              name, latencies.p50Ms, latencies.p99Ms, latencies.maxMs, peakThreads);
}

}

TEST_CASE("short work keeps its latency while long work saturates its lane")
{
  ThreadCounter perCall;
  const Submit spawn = [&perCall](std::function<void()> job) {
    std::thread([&perCall, job = std::move(job)] {
      perCall.enter();
      job();
      perCall.leave();
    }).detach();
  };
  const auto unbounded = shortJobsUnderLoad({.heavy = spawn, .light = spawn});
  const auto settle = Clock::now() + 5s;
  while (perCall.live.load() > 0 && Clock::now() < settle)
    std::this_thread::sleep_for(1ms);

  ElasticPool shared({.coreThreads = 2, .maxThreads = 8, .keepAlive = 5s});
  const Submit sharedSubmit = [&shared](std::function<void()> job) {
    shared.submit(std::move(job));
  };
  const auto oneLane = shortJobsUnderLoad({.heavy = sharedSubmit, .light = sharedSubmit});

  ElasticPool heavy({.coreThreads = 1, .maxThreads = 8, .keepAlive = 5s});
  ElasticPool light({.coreThreads = 2, .maxThreads = 64, .keepAlive = 5s});
  const auto twoLanes = shortJobsUnderLoad(
      {.heavy = [&heavy](std::function<void()> job) { heavy.submit(std::move(job)); },
       .light = [&light](std::function<void()> job) { light.submit(std::move(job)); }});

  report("thread per call", unbounded, perCall.peak.load());
  report("one bounded pool (8)", oneLane, shared.stats().peakThreads);
  report("heavy 8 + light 64 lanes", twoLanes,
         heavy.stats().peakThreads + light.stats().peakThreads);

  CHECK(heavy.stats().peakThreads <= 8);
  CHECK(light.stats().peakThreads <= 64);
  CHECK(twoLanes.p99Ms < 50.0);
  CHECK(oneLane.p99Ms > twoLanes.p99Ms);
  CHECK(perCall.peak.load() > heavy.stats().peakThreads + light.stats().peakThreads);
}

TEST_CASE("a reused worker is cheaper than a thread per call")
{
  constexpr int kJobs = 20000;

  std::atomic<int> perCallDone{0};
  const auto perCallStart = Clock::now();
  for (int i = 0; i < kJobs; ++i)
    std::thread([&perCallDone] { perCallDone.fetch_add(1); }).detach();
  REQUIRE(waitFor(perCallDone, kJobs, 60s));
  const double perCallMs =
      std::chrono::duration<double, std::milli>(Clock::now() - perCallStart).count();

  ElasticPool pool({.coreThreads = 4, .maxThreads = 64, .keepAlive = 5s});
  std::atomic<int> pooledDone{0};
  const auto pooledStart = Clock::now();
  for (int i = 0; i < kJobs; ++i)
    pool.submit([&pooledDone] { pooledDone.fetch_add(1); });
  REQUIRE(waitFor(pooledDone, kJobs, 60s));
  const double pooledMs =
      std::chrono::duration<double, std::milli>(Clock::now() - pooledStart).count();

  std::printf("%d empty jobs: thread per call %.1f ms (%.2f us/job), pool %.1f ms (%.2f us/job), peak pool threads %d\n",
              kJobs, perCallMs, perCallMs * 1000.0 / kJobs, pooledMs,
              pooledMs * 1000.0 / kJobs, pool.stats().peakThreads);

  CHECK(pool.stats().peakThreads <= 64);
  CHECK(pooledMs < perCallMs);
}

TEST_CASE("a burst of slow calls is bounded where a thread per call is not")
{
  constexpr int kCalls = 2000;
  constexpr auto kCall = 20ms;

  ThreadCounter perCall;
  std::atomic<int> perCallDone{0};
  const auto perCallStart = Clock::now();
  for (int i = 0; i < kCalls; ++i) {
    std::thread([&perCall, &perCallDone, kCall] {
      perCall.enter();
      std::this_thread::sleep_for(kCall);
      perCall.leave();
      perCallDone.fetch_add(1);
    }).detach();
  }
  REQUIRE(waitFor(perCallDone, kCalls, 60s));
  const double perCallMs =
      std::chrono::duration<double, std::milli>(Clock::now() - perCallStart).count();

  ElasticPool pool({.coreThreads = 4, .maxThreads = 64, .keepAlive = 5s});
  std::atomic<int> pooledDone{0};
  const auto pooledStart = Clock::now();
  for (int i = 0; i < kCalls; ++i) {
    pool.submit([&pooledDone, kCall] {
      std::this_thread::sleep_for(kCall);
      pooledDone.fetch_add(1);
    });
  }
  REQUIRE(waitFor(pooledDone, kCalls, 60s));
  const double pooledMs =
      std::chrono::duration<double, std::milli>(Clock::now() - pooledStart).count();

  std::printf("%d calls of 20 ms: thread per call peak %d threads in %.0f ms, pool peak %d threads in %.0f ms\n",
              kCalls, perCall.peak.load(), perCallMs, pool.stats().peakThreads, pooledMs);

  CHECK(pool.stats().peakThreads <= 64);
  CHECK(perCall.peak.load() > 64);
}
