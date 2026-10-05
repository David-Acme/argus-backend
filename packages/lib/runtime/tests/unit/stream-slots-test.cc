#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <runtime/stream-slots.hxx>

#include <atomic>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

TEST_CASE("a full set of slots refuses the next stream until a lease goes")
{
  StreamSlots slots(2);
  auto first = slots.tryAcquire();
  auto second = slots.tryAcquire();
  CHECK(second.has_value());
  CHECK(first.has_value());
  if (!first)
    return;
  CHECK_FALSE(slots.tryAcquire().has_value());
  CHECK(slots.active() == 2);

  first->release();
  CHECK(slots.active() == 1);
  first->release();
  CHECK(slots.active() == 1);

  auto third = slots.tryAcquire();
  CHECK(third.has_value());
  CHECK_FALSE(slots.tryAcquire().has_value());
}

TEST_CASE("a lease gives its slot back when it is destroyed, once, wherever it moved")
{
  StreamSlots slots(1);
  {
    auto lease = slots.tryAcquire();
    CHECK(lease.has_value());
    if (!lease)
      return;
    StreamLease moved = std::move(*lease);
    lease.reset();
    CHECK(slots.active() == 1);
    CHECK_FALSE(slots.drained());
  }
  CHECK(slots.drained());
  CHECK(slots.tryAcquire().has_value());
  CHECK(slots.drained());
}

TEST_CASE("a stop refuses new streams, tells the running ones and drains when they end")
{
  StreamSlots slots(3);
  auto running = slots.tryAcquire();
  CHECK(running.has_value());
  if (!running)
    return;
  CHECK_FALSE(running->stopping());

  slots.requestStop();
  CHECK(running->stopping());
  CHECK_FALSE(slots.tryAcquire().has_value());
  CHECK_FALSE(slots.drained());

  running.reset();
  CHECK(slots.drained());
}

TEST_CASE("a capacity below one still admits one stream")
{
  StreamSlots slots(0);
  CHECK(slots.capacity() == 1);
  auto only = slots.tryAcquire();
  CHECK(only.has_value());
  CHECK_FALSE(slots.tryAcquire().has_value());
}

TEST_CASE("concurrent acquirers never hold more slots than the capacity")
{
  constexpr int kCapacity = 3;
  constexpr int kThreads = 8;
  constexpr int kRounds = 2000;
  StreamSlots slots(kCapacity);
  std::atomic<int> peak{0};
  std::atomic<int> held{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([&] {
      for (int round = 0; round < kRounds; ++round) {
        auto lease = slots.tryAcquire();
        if (!lease)
          continue;
        const int now = held.fetch_add(1) + 1;
        int seen = peak.load();
        while (now > seen && !peak.compare_exchange_weak(seen, now)) {
        }
        held.fetch_sub(1);
      }
    });
  for (auto& thread : threads)
    thread.join();
  CHECK(peak.load() <= kCapacity);
  CHECK(slots.drained());
}
