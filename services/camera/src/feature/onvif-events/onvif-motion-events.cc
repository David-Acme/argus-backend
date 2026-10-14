#include <feature/onvif-events/onvif-motion-events.hxx>

#include <shared/services/camera-driver/onvif-driver.hxx>
#include <shared/services/onvif/onvif-transport.hxx>

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>
#include <utility>

namespace
{
int64_t wallMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool sameEndpoint(const OnvifEndpoint& left, const OnvifEndpoint& right)
{
  return left.host == right.host && left.port == right.port && left.username == right.username &&
         left.password == right.password;
}
}

OnvifMotionEvents::OnvifMotionEvents(OnvifMotionEventsParts parts)
    : parts_(std::move(parts)), thread_("onvif-events")
{
}

trantor::EventLoop* OnvifMotionEvents::loop()
{
  return thread_.getLoop();
}

std::vector<OnvifEventsTarget> OnvifMotionEvents::targetsOf(const std::vector<CameraSchema>& cameras)
{
  std::vector<OnvifEventsTarget> targets;
  for (const auto& camera : cameras) {
    if (camera.driver != CameraDriver::Onvif || !camera.isEnabled || camera.ip.empty())
      continue;
    targets.push_back({.cameraId = camera.id, .endpoint = OnvifDriver::endpointOf(camera)});
  }
  return targets;
}

void OnvifMotionEvents::start()
{
  const std::scoped_lock lock(lifecycleMutex_);
  if (stopRequested_ || started_)
    return;
  started_ = true;
  thread_.run();
  loop()->runInLoop([this] {
    drogon::async_run([this] { return reconcile(); });
    loop()->runEvery(kReconcileSeconds, [this] { drogon::async_run([this] { return reconcile(); }); });
  });
}

void OnvifMotionEvents::requestStop()
{
  const std::scoped_lock lock(lifecycleMutex_);
  if (stopRequested_.exchange(true))
    return;
  stopAtMs_ = wallMs();
  if (!started_)
    return;
  loop()->runInLoop([this] {
    for (const auto& [cameraId, watch] : watches_)
      watch->stop();
  });
}

bool OnvifMotionEvents::drained() const
{
  if (!stopRequested_)
    return false;
  const std::scoped_lock lock(allMutex_);
  if (std::ranges::all_of(all_, [](const auto& watch) { return watch->finished(); }))
    return true;
  if (wallMs() - stopAtMs_ < kStopBudgetMs)
    return false;
  if (!abandonLogged_.exchange(true))
    LOG_WARN << "ONVIF events: an Unsubscribe did not finish within " << kStopBudgetMs
             << " ms; the camera ends that subscription itself when it expires";
  return true;
}

drogon::Task<void> OnvifMotionEvents::reconcile()
{
  if (reconciling_ || stopRequested_)
    co_return;
  reconciling_ = true;
  std::optional<std::vector<OnvifEventsTarget>> targets;
  if (parts_.active && !parts_.active()) {
    targets.emplace();
  }
  else {
    try {
      targets = targetsOf(co_await repository_.findLive());
    }
    catch (const std::exception& error) {
      LOG_WARN << "ONVIF events: could not list the cameras (" << error.what() << ")";
    }
  }
  co_await drogon::switchThreadCoro(loop());
  reconciling_ = false;
  if (targets && !stopRequested_)
    apply(*targets);
}

void OnvifMotionEvents::apply(const std::vector<OnvifEventsTarget>& targets)
{
  std::erase_if(retiring_, [](const auto& entry) { return entry.second->finished(); });
  std::unordered_map<int64_t, const OnvifEventsTarget*> wanted;
  for (const auto& target : targets)
    wanted.emplace(target.cameraId, &target);
  std::erase_if(watches_, [this, &wanted](const auto& entry) {
    const auto& [cameraId, watch] = entry;
    const auto found = wanted.find(cameraId);
    const bool current = found != wanted.end() && sameEndpoint(found->second->endpoint, watch->endpoint()) &&
                         (!watch->finished() || watch->parked());
    if (current)
      return false;
    watch->stop();
    if (!watch->finished())
      retiring_.insert_or_assign(cameraId, watch);
    return true;
  });
  for (const auto& target : targets) {
    if (watches_.contains(target.cameraId) || retiring_.contains(target.cameraId))
      continue;
    auto watch = std::make_shared<OnvifMotionWatch>(OnvifMotionWatchParts{
        .cameraId = target.cameraId,
        .endpoint = target.endpoint,
        .transport = std::make_shared<OnvifAsyncHttpTransport>(target.endpoint, loop()),
        .board = parts_.board,
        .loop = loop(),
        .clockMs = wallMs,
        .firstBackoffMs = OnvifMotionWatch::kFirstBackoffMs,
        .maxBackoffMs = OnvifMotionWatch::kMaxBackoffMs,
        .minPullGapMs = OnvifMotionWatch::kMinPullGapMs});
    watches_.emplace(target.cameraId, watch);
    track(watch);
    watch->start();
  }
}

void OnvifMotionEvents::track(const std::shared_ptr<OnvifMotionWatch>& watch)
{
  const std::scoped_lock lock(allMutex_);
  std::erase_if(all_, [](const auto& known) { return known->finished(); });
  all_.push_back(watch);
}
