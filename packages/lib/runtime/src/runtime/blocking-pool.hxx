#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

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
};

struct BlockingLaneStats
{
  int threads{0};
  int idle{0};
  std::size_t queued{0};
  int peakThreads{0};
  std::uint64_t completed{0};
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

  [[nodiscard]] BlockingLaneStats stats() const;

  [[nodiscard]] const BlockingLaneLimits& limits() const { return limits_; }

private:
  BlockingLaneLimits limits_;
  std::shared_ptr<State> state_;
};

namespace blocking_pool
{

[[nodiscard]] BlockingLaneLimits limitsFor(BlockingLane lane);

ElasticPool& lane(BlockingLane lane);

void submit(BlockingLane lane, std::function<void()> job);

[[nodiscard]] BlockingLaneStats statsOf(BlockingLane lane);

}

class BlockingStrand
{
public:
  struct State;

  explicit BlockingStrand(BlockingLane lane = BlockingLane::Light);

  explicit BlockingStrand(ElasticPool& pool);

  void post(std::function<void()> job);

private:
  std::shared_ptr<State> state_;
};
