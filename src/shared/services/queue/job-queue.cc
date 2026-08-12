#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <drogon/drogon.h>
#include <shared/services/queue/job-queue.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <utility>

namespace
{

using SteadyNs = std::chrono::steady_clock::duration;

int64_t steadyNowNs()
{
  return std::chrono::steady_clock::now().time_since_epoch().count();
}

int64_t epochNow()
{
  return static_cast<int64_t>(
      std::chrono::system_clock::now().time_since_epoch().count() /
      1000000000LL);
}

int backoffMsFor(int priorFailures, int baseMs)
{
  int64_t value = static_cast<int64_t>(baseMs) * (1LL << priorFailures);
  value = std::min<int64_t>(value, 300000);
  const double jitter =
      0.8 + (static_cast<int64_t>(priorFailures) * 2654435761ULL % 41) / 100.0;
  return static_cast<int>(value * jitter);
}

struct PendingJob
{
  int priority = 0;
  int64_t id = 0;
  int64_t runAt = 0;
  int attempts = 0;
  int maxAttempts = 3;
  int backoffMs = 1000;
  int64_t createdAt = 0;
  std::string payload;
};

struct Better
{
  bool operator()(const PendingJob& a, const PendingJob& b) const
  {
    if (a.priority != b.priority)
      return a.priority < b.priority;
    return a.id > b.id;
  }
};

} // namespace

struct QueueManager::Queue
{
public:
  Queue(const QueueConfig& config, JobHandler handler, JobRepository& repo)
      : config_(config), handler_(std::move(handler)), repo_(repo),
        workerCount_(config.workers > 0
                         ? config.workers
                         : ThreadBudget::queueWorkers(config.name))
  {
    heap_.reserve(4096);
  }

  QueueConfig config_;
  JobHandler handler_;
  JobRepository& repo_;
  int workerCount_;

  int64_t add(const std::string& payload, const JobOptions& options)
  {
    const bool durable = config_.durable && options.durable;
    int64_t id = -1;
    if (durable) {
      if (!options.dedupeKey.empty()) {
        const auto existing =
            repo_.findIdByDedupe(config_.name, options.dedupeKey);
        if (existing)
          return *existing;
      }
      const job_query::JobInsertInput input = {
          .queue = config_.name,
          .payload = payload,
          .priority = options.priority,
          .maxAttempts = options.maxAttempts,
          .dedupeKey = options.dedupeKey,
          .nextRunAt =
              options.delayMs > 0 ? epochNow() + options.delayMs / 1000 : 0,
          .createdAt = epochNow(),
      };
      const auto inserted = repo_.insert(input);
      if (!inserted) {
        LOG_WARN << "QueueManager: durable insert failed for " << config_.name;
        return -1;
      }
      id = *inserted;
    }
    else {
      std::lock_guard<std::mutex> lock(mutex_);
      id = ++localId_;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      PendingJob job = {
          .priority = options.priority,
          .id = id,
          .runAt = options.delayMs > 0
                       ? steadyNowNs() +
                             static_cast<int64_t>(options.delayMs) * 1000000LL
                       : 0,
          .attempts = 0,
          .maxAttempts = options.maxAttempts,
          .backoffMs = options.backoffMs,
          .createdAt = epochNow(),
          .payload = payload,
      };
      heap_.push_back(std::move(job));
      std::push_heap(heap_.begin(), heap_.end(), Better{});
    }
    cv_.notify_all();
    return id;
  }

  void seedRecovered(const std::vector<job_query::JobRow>& rows)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& row : rows) {
      PendingJob job = {
          .priority = row.priority,
          .id = row.id,
          .runAt = 0,
          .attempts = row.attempts,
          .maxAttempts = row.maxAttempts,
          .backoffMs = 1000,
          .createdAt = row.createdAt,
          .payload = row.payload,
      };
      heap_.push_back(std::move(job));
    }
    if (!rows.empty())
      std::make_heap(heap_.begin(), heap_.end(), Better{});
  }

  void startWorkers()
  {
    for (int i = 0; i < workerCount_; ++i)
      workers_.emplace_back([this] { workerLoop(); });
  }

  void drain(int timeoutMs)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                      [this] { return heap_.empty() && active_ == 0; }))
      LOG_WARN << "QueueManager: drain timed out on " << config_.name;
  }

  void shutdown()
  {
    cancel_.cancel();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
      if (t.joinable())
        t.join();
    }
    workers_.clear();
  }

  QueueStats stats() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return {.waiting = heap_.size(),
            .active = active_,
            .completed = completed_,
            .failed = failed_,
            .delayed = delayedSeen_,
            .totalProcessed = completed_ + failed_};
  }

  void workerLoop()
  {
    for (;;) {
      PendingJob job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !heap_.empty(); });
        if (stop_ && heap_.empty() && active_ == 0)
          return;
        if (heap_.empty())
          continue;
        std::pop_heap(heap_.begin(), heap_.end(), Better{});
        job = std::move(heap_.back());
        heap_.pop_back();
        const int64_t now = steadyNowNs();
        if (job.runAt > now) {
          ++delayedSeen_;
          heap_.push_back(std::move(job));
          std::push_heap(heap_.begin(), heap_.end(), Better{});
          const auto deadline = std::chrono::steady_clock::time_point(
              std::chrono::steady_clock::duration{job.runAt});
          cv_.wait_until(lock, deadline, [this] {
            return stop_ ||
                   (!heap_.empty() && heap_.front().runAt <= steadyNowNs());
          });
          continue;
        }
        ++active_;
      }

      Job context = {
          .id = job.id,
          .queue = config_.name,
          .payload = job.payload,
          .attempts = job.attempts,
          .createdAt = job.createdAt,
          .cancel = cancel_,
      };
      JobResult result;
      try {
        result = handler_(context);
      }
      catch (const std::exception& e) {
        result = {.ok = false, .error = e.what()};
      }
      catch (...) {
        result = {.ok = false, .error = "unknown exception"};
      }

      const bool exhausted = !result.ok && job.attempts + 1 >= job.maxAttempts;
      const int nextAttempts = job.attempts + 1;
      if (config_.durable) {
        if (result.ok) {
          repo_.complete(job.id);
        }
        else if (exhausted) {
          repo_.markFailed(job.id, nextAttempts, result.error, epochNow());
        }
        else {
          const int backoff = backoffMsFor(job.attempts, job.backoffMs);
          repo_.markDelayed(job.id, nextAttempts, result.error,
                            epochNow() + backoff / 1000, epochNow());
        }
      }

      if (result.ok || exhausted) {
        std::lock_guard<std::mutex> lock(mutex_);
        --active_;
        if (result.ok)
          ++completed_;
        else
          ++failed_;
      }
      else {
        const int backoff = backoffMsFor(job.attempts, job.backoffMs);
        std::lock_guard<std::mutex> lock(mutex_);
        --active_;
        heap_.push_back(
            {.priority = job.priority,
             .id = job.id,
             .runAt = steadyNowNs() + static_cast<int64_t>(backoff) * 1000000LL,
             .attempts = nextAttempts,
             .maxAttempts = job.maxAttempts,
             .backoffMs = job.backoffMs,
             .createdAt = job.createdAt,
             .payload = job.payload});
        std::push_heap(heap_.begin(), heap_.end(), Better{});
      }
      cv_.notify_all();
    }
  }

  std::vector<PendingJob> heap_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  size_t active_ = 0;
  size_t completed_ = 0;
  size_t failed_ = 0;
  size_t delayedSeen_ = 0;
  int64_t localId_ = 0;
  std::vector<std::thread> workers_;
  CancellationToken cancel_;
};

QueueManager::QueueManager() = default;

QueueManager::~QueueManager()
{
  shutdown();
}

void QueueManager::registerQueue(const QueueConfig& config, JobHandler handler)
{
  queues_.push_back(
      std::make_unique<Queue>(config, std::move(handler), repository_));
}

int64_t QueueManager::add(const std::string& queue, const std::string& payload,
                          const JobOptions& options)
{
  for (const auto& q : queues_) {
    if (q->config_.name == queue)
      return q->add(payload, options);
  }
  LOG_WARN << "QueueManager: unknown queue " << queue;
  return -1;
}

bool QueueManager::start(const std::string& dbPath)
{
  if (started_)
    return true;
  started_ = true;
  if (!repository_.open(dbPath))
    return false;
  repository_.resetActive(epochNow());
  for (const auto& q : queues_) {
    const std::vector<job_query::JobRow> rows =
        repository_.loadDue(q->config_.name, epochNow());
    q->seedRecovered(rows);
  }
  for (const auto& q : queues_)
    q->startWorkers();
  return true;
}

void QueueManager::drain(int timeoutMs)
{
  for (const auto& q : queues_)
    q->drain(timeoutMs);
}

void QueueManager::shutdown()
{
  if (!started_)
    return;
  started_ = false;
  for (auto it = queues_.rbegin(); it != queues_.rend(); ++it)
    (*it)->shutdown();
  repository_.close();
}

QueueStats QueueManager::stats(const std::string& queue) const
{
  for (const auto& q : queues_) {
    if (q->config_.name == queue)
      return q->stats();
  }
  return {};
}
