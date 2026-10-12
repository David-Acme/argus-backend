#include "episode-change-gate.hxx"

#include <ctime>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/guard/controllers/guard-errors.hxx>
#include <feature/guard/infra/resource-change-outbox.hxx>
#include <string>
#include <trantor/utils/Logger.h>
#include <utility>
#include <vector>

namespace
{
struct PendingGuard
{
  explicit PendingGuard(std::atomic<int>* pending) : pending(pending) {}

  PendingGuard(const PendingGuard&) = delete;
  PendingGuard& operator=(const PendingGuard&) = delete;
  PendingGuard(PendingGuard&&) = delete;
  PendingGuard& operator=(PendingGuard&&) = delete;
  ~PendingGuard() { pending->fetch_sub(1, std::memory_order_acq_rel); }

  std::atomic<int>* pending;
};
}

EpisodeChangeGate::EpisodeChangeGate(const ResourceChangeOutbox& changes,
                                     Config config)
    : changes_(&changes), config_(std::move(config))
{
  if (!config_.clock)
    config_.clock = []() {
      return static_cast<int64_t>(std::time(nullptr));
    };
}

int64_t EpisodeChangeGate::now() const
{
  return config_.clock();
}

bool EpisodeChangeGate::dueLocked(int64_t encounterId, int64_t at) const
{
  const auto published = publishedAt_.find(encounterId);
  if (published == publishedAt_.end())
    return true;
  return static_cast<double>(at - published->second) >= config_.publishSeconds;
}

drogon::Task<void> EpisodeChangeGate::touch(int64_t encounterId)
{
  if (encounterId <= 0 || changes_ == nullptr)
    co_return;
  const int64_t at = now();
  {
    std::scoped_lock lock(mutex_);
    if (!dueLocked(encounterId, at)) {
      dirty_.insert(encounterId);
      co_return;
    }
  }
  co_await changes_->record({.scope = ResourceScope::Episodes,
                             .ids = {encounterId},
                             .client = nullptr});
  std::scoped_lock lock(mutex_);
  publishedAt_[encounterId] = at;
  dirty_.erase(encounterId);
}

void EpisodeChangeGate::published(int64_t encounterId)
{
  if (encounterId <= 0)
    return;
  std::scoped_lock lock(mutex_);
  publishedAt_[encounterId] = now();
  dirty_.erase(encounterId);
}

drogon::Task<void> EpisodeChangeGate::flush()
{
  if (changes_ == nullptr)
    co_return;
  const int64_t at = now();
  std::vector<int64_t> ready;
  {
    std::scoped_lock lock(mutex_);
    for (const auto id : dirty_) {
      if (dueLocked(id, at))
        ready.push_back(id);
    }
  }
  if (ready.empty())
    co_return;
  try {
    co_await changes_->record(
        {.scope = ResourceScope::Episodes, .ids = ready, .client = nullptr});
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard episodes: the trailing changes for " << ready.size()
             << " episode(s) were not recorded: " << error.what();
    co_return;
  }
  std::scoped_lock lock(mutex_);
  for (const auto id : ready) {
    publishedAt_[id] = at;
    dirty_.erase(id);
  }
}

void EpisodeChangeGate::start()
{
  if (timer_.has_value())
    return;
  schedule();
  timer_ = drogon::app().getLoop()->runEvery(5.0, [this]() { schedule(); });
}

void EpisodeChangeGate::requestStop()
{
  stopped_.store(true, std::memory_order_release);
  if (timer_.has_value() && drogon::app().isRunning())
    drogon::app().getLoop()->invalidateTimer(*timer_);
  timer_.reset();
}

bool EpisodeChangeGate::drained() const
{
  return pending_.load(std::memory_order_acquire) == 0;
}

void EpisodeChangeGate::schedule()
{
  if (stopped_.load(std::memory_order_acquire))
    return;
  pending_.fetch_add(1, std::memory_order_acq_rel);
  drogon::async_run([this]() -> drogon::Task<void> {
    const PendingGuard guard{&pending_};
    co_await flush();
  });
}
