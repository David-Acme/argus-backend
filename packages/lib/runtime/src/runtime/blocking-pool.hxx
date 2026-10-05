#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>

enum class BlockingLane : std::uint8_t
{
  Light,
  Heavy,
};

struct BlockingLaneLimits
{
  int coreThreads{1};
  int maxThreads{1};
  std::chrono::milliseconds keepAlive{std::chrono::seconds(30)};
  std::size_t maxQueued{0};
};

struct BlockingLaneStats
{
  int threads{0};
  int idle{0};
  std::size_t queued{0};
  int peakThreads{0};
  std::uint64_t completed{0};
  std::uint64_t rejected{0};
  std::chrono::milliseconds oldestQueuedAge{0};
};

enum class BlockingAdmission : std::uint8_t
{
  Queue,
  RejectWhenFull,
};

class BlockingLaneFull : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

class ElasticPool
{
public:
  struct State;

  explicit ElasticPool(const BlockingLaneLimits& limits);
  ~ElasticPool();

  ElasticPool(const ElasticPool&) = delete;
  ElasticPool& operator=(const ElasticPool&) = delete;
  ElasticPool(ElasticPool&&) = delete;
  ElasticPool& operator=(ElasticPool&&) = delete;

  void submit(std::function<void()> job);

  [[nodiscard]] bool trySubmit(std::function<void()> job);

  [[nodiscard]] BlockingLaneStats stats() const;

  [[nodiscard]] BlockingLaneStats
  stats(std::chrono::steady_clock::time_point now) const;

  [[nodiscard]] const BlockingLaneLimits& limits() const { return limits_; }

private:
  bool enqueue(std::function<void()> job, BlockingAdmission admission);

  BlockingLaneLimits limits_;
  std::shared_ptr<State> state_;
};

namespace blocking_pool
{

[[nodiscard]] BlockingLaneLimits limitsFor(BlockingLane lane);

ElasticPool& lane(BlockingLane lane);

void submit(BlockingLane lane, std::function<void()> job);

[[nodiscard]] bool trySubmit(BlockingLane lane, std::function<void()> job);

[[nodiscard]] BlockingLaneStats statsOf(BlockingLane lane);

}

class BlockingStrand
{
public:
  struct State;

  explicit BlockingStrand(BlockingLane lane = BlockingLane::Light,
                          std::size_t maxQueued = 0);

  explicit BlockingStrand(ElasticPool& pool, std::size_t maxQueued = 0);

  void post(std::function<void()> job);

  [[nodiscard]] bool tryPost(std::function<void()> job);

  [[nodiscard]] std::size_t queued() const;

private:
  std::shared_ptr<State> state_;
};
