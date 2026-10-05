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
  struct Queued
  {
    std::function<void()> job;
    std::chrono::steady_clock::time_point enqueuedAt;
  };

  explicit State(const BlockingLaneLimits& laneLimits) : limits(laneLimits) {}

  BlockingLaneLimits limits;
  mutable std::mutex mutex;
  std::condition_variable ready;
  std::deque<Queued> queue;
  int threads{0};
  int idle{0};
  int peakThreads{0};
  std::uint64_t completed{0};
  std::uint64_t rejected{0};
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
    auto job = std::move(state->queue.front().job);
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
              .keepAlive = limits.keepAlive,
              .maxQueued = limits.maxQueued},
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
  static_cast<void>(enqueue(std::move(job), BlockingAdmission::Queue));
}

bool ElasticPool::trySubmit(std::function<void()> job)
{
  return enqueue(std::move(job), BlockingAdmission::RejectWhenFull);
}

bool ElasticPool::enqueue(std::function<void()> job, BlockingAdmission admission)
{
  std::unique_lock lock(state_->mutex);
  if (admission == BlockingAdmission::RejectWhenFull && limits_.maxQueued > 0 &&
      state_->queue.size() >= limits_.maxQueued) {
    ++state_->rejected;
    return false;
  }
  state_->queue.push_back(
      {.job = std::move(job), .enqueuedAt = std::chrono::steady_clock::now()});
  const auto waiting = static_cast<int>(state_->queue.size());
  if (waiting <= state_->idle || state_->threads >= limits_.maxThreads) {
    lock.unlock();
    state_->ready.notify_one();
    return true;
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
  return true;
}

BlockingLaneStats ElasticPool::stats() const
{
  return stats(std::chrono::steady_clock::now());
}

BlockingLaneStats ElasticPool::stats(std::chrono::steady_clock::time_point now) const
{
  std::scoped_lock lock(state_->mutex);
  std::chrono::milliseconds oldest{0};
  if (!state_->queue.empty())
    oldest = std::max(std::chrono::milliseconds{0},
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - state_->queue.front().enqueuedAt));
  return {.threads = state_->threads,
          .idle = state_->idle,
          .queued = state_->queue.size(),
          .peakThreads = state_->peakThreads,
          .completed = state_->completed,
          .rejected = state_->rejected,
          .oldestQueuedAge = oldest};
}

namespace
{

union ImmortalLane
{
  explicit ImmortalLane(const BlockingLaneLimits& limits) : pool(limits) {}
  ImmortalLane(const ImmortalLane&) = delete;
  ImmortalLane& operator=(const ImmortalLane&) = delete;
  ImmortalLane(ImmortalLane&&) = delete;
  ImmortalLane& operator=(ImmortalLane&&) = delete;
  ~ImmortalLane() {}

  ElasticPool pool;
};

}

namespace blocking_pool
{

BlockingLaneLimits limitsFor(BlockingLane lane)
{
  if (lane == BlockingLane::Heavy)
    return {.coreThreads = 1,
            .maxThreads = ThreadBudget::blockingHeavyThreads(),
            .keepAlive = std::chrono::seconds(30),
            .maxQueued = static_cast<std::size_t>(
                ThreadBudget::blockingHeavyThreads()) * 16};
  return {.coreThreads = ThreadBudget::lightThreads(),
          .maxThreads = ThreadBudget::blockingLightThreads(),
          .keepAlive = std::chrono::seconds(30),
          .maxQueued = static_cast<std::size_t>(
              ThreadBudget::blockingLightThreads()) * 64};
}

ElasticPool& lane(BlockingLane lane)
{
  if (lane == BlockingLane::Heavy) {
    static ImmortalLane heavy(limitsFor(BlockingLane::Heavy));
    return heavy.pool;
  }
  static ImmortalLane light(limitsFor(BlockingLane::Light));
  return light.pool;
}

void submit(BlockingLane laneName, std::function<void()> job)
{
  lane(laneName).submit(std::move(job));
}

bool trySubmit(BlockingLane laneName, std::function<void()> job)
{
  return lane(laneName).trySubmit(std::move(job));
}

BlockingLaneStats statsOf(BlockingLane laneName)
{
  return lane(laneName).stats();
}

}

struct BlockingStrand::State
{
  State(ElasticPool& strandPool, std::size_t strandMaxQueued)
      : pool(&strandPool), maxQueued(strandMaxQueued)
  {
  }

  ElasticPool* pool;
  std::size_t maxQueued;
  mutable std::mutex mutex;
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
  for (;;) {
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
    try {
      scheduleStrand(state);
      return;
    }
    catch (const std::exception& error) {
      LOG_WARN << "Blocking strand: could not hand the next job to the pool ("
               << error.what() << "), running it on this worker";
    }
  }
}

}

BlockingStrand::BlockingStrand(BlockingLane lane, std::size_t maxQueued)
    : BlockingStrand(blocking_pool::lane(lane), maxQueued)
{
}

BlockingStrand::BlockingStrand(ElasticPool& pool, std::size_t maxQueued)
    : state_(std::make_shared<State>(pool, maxQueued))
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
  try {
    scheduleStrand(state_);
  }
  catch (...) {
    bool remaining = false;
    {
      std::scoped_lock lock(state_->mutex);
      state_->queue.pop_front();
      remaining = !state_->queue.empty();
      state_->scheduled = remaining;
    }
    if (remaining) {
      try {
        scheduleStrand(state_);
      }
      catch (...) {
        std::scoped_lock lock(state_->mutex);
        state_->scheduled = false;
      }
    }
    throw;
  }
}

bool BlockingStrand::tryPost(std::function<void()> job)
{
  {
    std::scoped_lock lock(state_->mutex);
    if (state_->maxQueued > 0 && state_->queue.size() >= state_->maxQueued)
      return false;
  }
  post(std::move(job));
  return true;
}

std::size_t BlockingStrand::queued() const
{
  std::scoped_lock lock(state_->mutex);
  return state_->queue.size();
}
