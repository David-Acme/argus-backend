#include "blocking-pool.hxx"

#include "thread-budget.hxx"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <trantor/utils/Logger.h>
#include <utility>

struct ElasticPool::State
{
  explicit State(const BlockingLaneLimits& laneLimits) : limits(laneLimits) {}

  BlockingLaneLimits limits;
  mutable std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::function<void()>> queue;
  int threads{0};
  int idle{0};
  int peakThreads{0};
  std::uint64_t completed{0};
  bool stopping{false};
};

namespace
{

void runJob(const std::function<void()>& job) noexcept
{
  try {
    job();
  }
  catch (const std::exception& error) {
    LOG_ERROR << "Blocking pool: a job escaped with " << error.what();
  }
  catch (...) {
    LOG_ERROR << "Blocking pool: a job escaped with an unknown exception";
  }
}

void work(const std::shared_ptr<ElasticPool::State>& state)
{
  std::unique_lock lock(state->mutex);
  for (;;) {
    ++state->idle;
    const bool woke = state->ready.wait_for(
        lock, state->limits.keepAlive,
        [&state] { return state->stopping || !state->queue.empty(); });
    --state->idle;
    if (state->queue.empty()) {
      if (state->stopping || (!woke && state->threads > state->limits.coreThreads)) {
        --state->threads;
        return;
      }
      continue;
    }
    auto job = std::move(state->queue.front());
    state->queue.pop_front();
    lock.unlock();
    runJob(job);
    job = nullptr;
    lock.lock();
    ++state->completed;
  }
}

}

ElasticPool::ElasticPool(const BlockingLaneLimits& limits)
    : limits_{.coreThreads = std::max(0, limits.coreThreads),
              .maxThreads = std::max({1, limits.maxThreads, limits.coreThreads}),
              .keepAlive = limits.keepAlive},
      state_(std::make_shared<State>(limits_))
{
}

ElasticPool::~ElasticPool()
{
  {
    std::scoped_lock lock(state_->mutex);
    state_->stopping = true;
  }
  state_->ready.notify_all();
}

void ElasticPool::submit(std::function<void()> job)
{
  std::unique_lock lock(state_->mutex);
  state_->queue.push_back(std::move(job));
  const auto waiting = static_cast<int>(state_->queue.size());
  if (waiting <= state_->idle || state_->threads >= limits_.maxThreads) {
    lock.unlock();
    state_->ready.notify_one();
    return;
  }
  try {
    std::thread(work, state_).detach();
    ++state_->threads;
    state_->peakThreads = std::max(state_->peakThreads, state_->threads);
  }
  catch (const std::system_error& error) {
    LOG_WARN << "Blocking pool: could not start a worker (" << error.what()
             << "), the job waits for a busy one";
    if (state_->threads == 0) {
      state_->queue.pop_back();
      throw;
    }
  }
}

BlockingLaneStats ElasticPool::stats() const
{
  std::scoped_lock lock(state_->mutex);
  return {.threads = state_->threads,
          .idle = state_->idle,
          .queued = state_->queue.size(),
          .peakThreads = state_->peakThreads,
          .completed = state_->completed};
}

namespace blocking_pool
{

BlockingLaneLimits limitsFor(BlockingLane lane)
{
  if (lane == BlockingLane::Heavy)
    return {.coreThreads = 1,
            .maxThreads = ThreadBudget::blockingHeavyThreads(),
            .keepAlive = std::chrono::seconds(30)};
  return {.coreThreads = ThreadBudget::lightThreads(),
          .maxThreads = ThreadBudget::blockingLightThreads(),
          .keepAlive = std::chrono::seconds(30)};
}

ElasticPool& lane(BlockingLane lane)
{
  if (lane == BlockingLane::Heavy) {
    static ElasticPool heavy(limitsFor(BlockingLane::Heavy));
    return heavy;
  }
  static ElasticPool light(limitsFor(BlockingLane::Light));
  return light;
}

void submit(BlockingLane laneName, std::function<void()> job)
{
  lane(laneName).submit(std::move(job));
}

BlockingLaneStats statsOf(BlockingLane laneName)
{
  return lane(laneName).stats();
}

}

struct BlockingStrand::State
{
  explicit State(ElasticPool& strandPool) : pool(&strandPool) {}

  ElasticPool* pool;
  std::mutex mutex;
  std::deque<std::function<void()>> queue;
  bool scheduled{false};
};

namespace
{

void drainStrand(const std::shared_ptr<BlockingStrand::State>& state);

void scheduleStrand(const std::shared_ptr<BlockingStrand::State>& state)
{
  state->pool->submit([state] { drainStrand(state); });
}

void drainStrand(const std::shared_ptr<BlockingStrand::State>& state)
{
  std::function<void()> job;
  {
    std::scoped_lock lock(state->mutex);
    job = std::move(state->queue.front());
    state->queue.pop_front();
  }
  runJob(job);
  {
    std::scoped_lock lock(state->mutex);
    if (state->queue.empty()) {
      state->scheduled = false;
      return;
    }
  }
  scheduleStrand(state);
}

}

BlockingStrand::BlockingStrand(BlockingLane lane)
    : BlockingStrand(blocking_pool::lane(lane))
{
}

BlockingStrand::BlockingStrand(ElasticPool& pool)
    : state_(std::make_shared<State>(pool))
{
}

void BlockingStrand::post(std::function<void()> job)
{
  {
    std::scoped_lock lock(state_->mutex);
    state_->queue.push_back(std::move(job));
    if (state_->scheduled)
      return;
    state_->scheduled = true;
  }
  scheduleStrand(state_);
}
