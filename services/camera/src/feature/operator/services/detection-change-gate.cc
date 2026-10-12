#include "detection-change-gate.hxx"

#include <ctime>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <string>
#include <sync/camera-change-sink.hxx>
#include <sync/resource-change.hxx>
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

constexpr double kTickSeconds = 5.0;
}

DetectionChangeGate::DetectionChangeGate(Config config)
    : config_(std::move(config))
{
  if (!config_.clock)
    config_.clock = []() {
      return static_cast<int64_t>(std::time(nullptr));
    };
}

int64_t DetectionChangeGate::now() const
{
  return config_.clock();
}

bool DetectionChangeGate::dueLocked(const DueInput& input) const
{
  const auto published = publishedAt_.find(input.cameraId);
  if (published == publishedAt_.end())
    return true;
  return static_cast<double>(input.at - published->second) >=
         config_.publishSeconds;
}

drogon::Task<void> DetectionChangeGate::detected(int64_t cameraId)
{
  if (cameraId <= 0)
    co_return;
  const int64_t at = now();
  {
    std::scoped_lock lock(mutex_);
    if (!dueLocked({.cameraId = cameraId, .at = at})) {
      dirty_.insert(cameraId);
      co_return;
    }
  }
  const CameraChangeSink* sink = camera_change::getSink();
  if (sink == nullptr)
    co_return;
  co_await sink->publishResource({.scope = ResourceScope::Overview,
                                  .ids = {cameraId},
                                  .user = 0,
                                  .client = nullptr});
  std::scoped_lock lock(mutex_);
  publishedAt_[cameraId] = at;
  dirty_.erase(cameraId);
}

drogon::Task<void> DetectionChangeGate::flush()
{
  const int64_t at = now();
  std::vector<int64_t> ready;
  {
    std::scoped_lock lock(mutex_);
    for (const auto cameraId : dirty_) {
      if (dueLocked({.cameraId = cameraId, .at = at}))
        ready.push_back(cameraId);
    }
  }
  if (ready.empty())
    co_return;
  const CameraChangeSink* sink = camera_change::getSink();
  if (sink == nullptr)
    co_return;
  try {
    co_await sink->publishResource(
        {.scope = ResourceScope::Overview, .ids = ready, .user = 0, .client = nullptr});
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera detections: the trailing changes for " << ready.size()
             << " camera(s) were not recorded: " << error.what();
    co_return;
  }
  std::scoped_lock lock(mutex_);
  for (const auto cameraId : ready) {
    publishedAt_[cameraId] = at;
    dirty_.erase(cameraId);
  }
}

void DetectionChangeGate::start()
{
  if (timer_.has_value())
    return;
  schedule();
  timer_ = drogon::app().getLoop()->runEvery(kTickSeconds, [this]() { schedule(); });
}

void DetectionChangeGate::requestStop()
{
  stopped_.store(true, std::memory_order_release);
  if (timer_.has_value() && drogon::app().isRunning())
    drogon::app().getLoop()->invalidateTimer(*timer_);
  timer_.reset();
}

bool DetectionChangeGate::drained() const
{
  return pending_.load(std::memory_order_acquire) == 0;
}

void DetectionChangeGate::schedule()
{
  if (stopped_.load(std::memory_order_acquire))
    return;
  pending_.fetch_add(1, std::memory_order_acq_rel);
  drogon::async_run([this]() -> drogon::Task<void> {
    const PendingGuard guard{&pending_};
    co_await flush();
  });
}
