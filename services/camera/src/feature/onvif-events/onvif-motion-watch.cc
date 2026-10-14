#include <feature/onvif-events/onvif-motion-watch.hxx>

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

class OnvifMotionWatch::Pause
{
public:
  Pause(OnvifMotionWatch& watch, int64_t ms) : watch_(watch), ms_(ms) {}

  [[nodiscard]] bool await_ready() const noexcept { return watch_.stopping_.load(); }

  void await_suspend(std::coroutine_handle<> handle)
  {
    watch_.paused_ = handle;
    watch_.pauseTimer_ = watch_.parts_.loop->runAfter(static_cast<double>(ms_) / 1000.0,
                                                      [self = watch_.shared_from_this()] { self->wake(); });
  }

  void await_resume() const noexcept {}

private:
  OnvifMotionWatch& watch_;
  int64_t ms_;
};

OnvifMotionWatch::OnvifMotionWatch(OnvifMotionWatchParts parts)
    : parts_(std::move(parts)),
      client_({.endpoint = parts_.endpoint, .transport = {}, .asyncTransport = parts_.transport, .clock = {}})
{
}

void OnvifMotionWatch::start()
{
  parts_.loop->runInLoop([self = shared_from_this()] { drogon::async_run([self] { return self->run(); }); });
}

void OnvifMotionWatch::stop()
{
  parts_.loop->runInLoop([self = shared_from_this()] {
    if (self->stopping_.exchange(true))
      return;
    self->parts_.transport->cancel();
    self->wake();
  });
}

bool OnvifMotionWatch::finished() const
{
  return finished_.load();
}

bool OnvifMotionWatch::parked() const
{
  return parked_.load();
}

const OnvifEndpoint& OnvifMotionWatch::endpoint() const
{
  return parts_.endpoint;
}

int64_t OnvifMotionWatch::nextBackoffMs(int64_t currentMs, int64_t maxMs)
{
  return std::min(currentMs * 2, maxMs);
}

int64_t OnvifMotionWatch::renewAfterMs(int64_t lifetimeSeconds)
{
  return (lifetimeSeconds > 0 ? lifetimeSeconds : kDefaultLifetimeSeconds) * 1000 / 2;
}

void OnvifMotionWatch::wake()
{
  if (!paused_)
    return;
  parts_.loop->invalidateTimer(pauseTimer_);
  std::exchange(paused_, {}).resume();
}

void OnvifMotionWatch::report(bool moving)
{
  if (parts_.board != nullptr)
    parts_.board->report({.cameraId = parts_.cameraId, .moving = moving, .atMs = parts_.clockMs()});
}

void OnvifMotionWatch::failed(const std::string& step, const std::string& error)
{
  if (std::exchange(failing_, true))
    return;
  LOG_WARN << "ONVIF events: camera " << parts_.cameraId << " failed " << step << " (" << error
           << "); it is tried again after a pause";
}

drogon::Task<void> OnvifMotionWatch::run()
{
  const auto keep = shared_from_this();
  int64_t backoffMs = parts_.firstBackoffMs;
  while (!stopping_) {
    const auto opened = co_await client_.openEvents();
    if (stopping_)
      break;
    if (!opened.ok()) {
      if (opened.failure == OnvifFailure::Unsupported) {
        parked_ = true;
        LOG_INFO << "ONVIF events: camera " << parts_.cameraId << " gets no motion subscription (" << opened.error
                 << ")";
        break;
      }
      failed("to open its events", opened.error);
      co_await Pause(*this, backoffMs);
      backoffMs = nextBackoffMs(backoffMs, parts_.maxBackoffMs);
      continue;
    }
    const auto made = co_await client_.subscribe();
    if (made.ok())
      subscription_ = made.value;
    if (stopping_)
      break;
    if (!made.ok()) {
      failed("to subscribe", made.error);
      co_await Pause(*this, backoffMs);
      backoffMs = nextBackoffMs(backoffMs, parts_.maxBackoffMs);
      continue;
    }
    if (co_await follow())
      backoffMs = parts_.firstBackoffMs;
    report(false);
    if (stopping_)
      break;
    co_await release();
    co_await Pause(*this, backoffMs);
    backoffMs = nextBackoffMs(backoffMs, parts_.maxBackoffMs);
  }
  co_await release();
  report(false);
  finished_ = true;
}

drogon::Task<bool> OnvifMotionWatch::follow()
{
  bool delivered = false;
  if (!subscription_)
    co_return delivered;
  int64_t renewAt = parts_.clockMs() + renewAfterMs(subscription_->lifetimeSeconds);
  std::optional<std::chrono::steady_clock::time_point> lastPull;
  while (!stopping_) {
    if (parts_.clockMs() >= renewAt) {
      const auto renewed = co_await client_.renew(*subscription_);
      if (!renewed.ok()) {
        if (!stopping_)
          failed("to renew its subscription", renewed.error);
        co_return delivered;
      }
      renewAt = parts_.clockMs() + renewAfterMs(renewed.value);
      continue;
    }
    if (lastPull) {
      const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - *lastPull);
      if (since.count() < parts_.minPullGapMs) {
        co_await Pause(*this, parts_.minPullGapMs - since.count());
        if (stopping_)
          break;
      }
    }
    lastPull = std::chrono::steady_clock::now();
    const auto pulled = co_await client_.pull(*subscription_);
    if (!pulled.ok()) {
      if (!stopping_)
        failed("to pull its messages", pulled.error);
      co_return delivered;
    }
    delivered = true;
    if (std::exchange(failing_, false))
      LOG_INFO << "ONVIF events: camera " << parts_.cameraId << " delivers its motion events again";
    const int64_t received = parts_.clockMs();
    if (parts_.board != nullptr) {
      for (const auto& motion : pulled.value.motions)
        parts_.board->report({.cameraId = parts_.cameraId, .moving = motion.moving, .atMs = received});
    }
  }
  co_return delivered;
}

drogon::Task<void> OnvifMotionWatch::release()
{
  const auto subscription = std::exchange(subscription_, std::nullopt);
  if (subscription)
    static_cast<void>(co_await client_.unsubscribe(*subscription));
}
