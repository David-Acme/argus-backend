#include "watch-coordinator.hxx"

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <utility>

namespace
{

constexpr auto kStageTwoGrace = std::chrono::milliseconds(1000);

}

bool broadcastBurst(const WakeDecisionInput& input)
{
  if (input.fires.empty())
    return false;
  std::vector<int64_t> cameras;
  float lowest = input.fires.front().snrDb;
  float highest = lowest;
  const auto first = input.fires.front().at;
  for (const EarFire& fire : input.fires) {
    if (fire.at - first > input.config.broadcastWindow)
      continue;
    if (std::ranges::find(cameras, fire.cameraId) == cameras.end())
      cameras.push_back(fire.cameraId);
    lowest = std::min(lowest, fire.snrDb);
    highest = std::max(highest, fire.snrDb);
  }
  return std::cmp_greater_equal(cameras.size(), input.config.broadcastMinCameras) &&
         highest - lowest <= input.config.broadcastSpreadDb;
}

WakeDecision decideWakes(const WakeDecisionInput& input)
{
  WakeDecision decision;
  if (broadcastBurst(input)) {
    decision.broadcast = true;
    return decision;
  }
  std::optional<size_t> best;
  std::vector<std::pair<size_t, float>> passing;
  for (size_t index = 0; index < input.fires.size(); ++index) {
    const EarFire& fire = input.fires[index];
    const auto camera = input.cameras.find(fire.cameraId);
    const bool tv = camera != input.cameras.end() && camera->second.tvExclusion;
    const float threshold = input.config.minWakeSnrDb + (tv ? input.config.tvRaiseDb : 0.0F);
    if (fire.snrDb < threshold) {
      decision.quiet.push_back(index);
      continue;
    }
    passing.emplace_back(index, threshold);
    if (!best || fire.snrDb > input.fires[*best].snrDb)
      best = index;
  }
  for (const auto& [index, threshold] : passing) {
    if (best && index == *best)
      decision.candidates.push_back({.fire = index, .threshold = threshold});
    else
      decision.duplicates.push_back(index);
  }
  return decision;
}

WatchCoordinator::WatchCoordinator(WatchCoordinatorInput input)
    : input_(std::move(input)),
      stageTwo_(*input_.stt),
      fireLog_(static_cast<size_t>(input_.config.fireLogSize))
{
}

WatchCoordinator::~WatchCoordinator()
{
  shutdown();
}

void WatchCoordinator::start()
{
  if (!input_.config.enabled || !input_.camera) {
    LOG_INFO << "Voice watch: the watchman's ear is off (watch.enabled or camera.grpc_target)";
    return;
  }
  worker_ = std::jthread([this](const std::stop_token& stop) { workerLoop(stop); });
  refresher_ = std::jthread([this](const std::stop_token& stop) { refreshLoop(stop); });
}

void WatchCoordinator::shutdown()
{
  if (refresher_.joinable()) {
    refresher_.request_stop();
    refresher_.join();
  }
  if (worker_.joinable()) {
    worker_.request_stop();
    queueReady_.notify_all();
    worker_.join();
  }
  std::map<int64_t, std::unique_ptr<CameraEar>> ears;
  {
    std::scoped_lock lock(mutex_);
    ears.swap(ears_);
  }
  ears.clear();
}

std::optional<WatchCameraInfo> WatchCoordinator::camera(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = cameras_.find(cameraId);
  if (found == cameras_.end())
    return std::nullopt;
  return found->second;
}

std::vector<WatchCameraInfo> WatchCoordinator::cameras() const
{
  std::vector<WatchCameraInfo> out;
  std::scoped_lock lock(mutex_);
  for (const auto& [id, info] : cameras_)
    out.push_back(info);
  return out;
}

float WatchCoordinator::snrOf(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = ears_.find(cameraId);
  return found == ears_.end() ? 0.0F : found->second->snrDb();
}

void WatchCoordinator::refreshLoop(const std::stop_token& stop)
{
  std::mutex waitMutex;
  std::condition_variable_any wake;
  while (!stop.stop_requested()) {
    refresh();
    std::unique_lock lock(waitMutex);
    static_cast<void>(wake.wait_for(lock, stop, input_.config.listRefresh, [] { return false; }));
  }
}

void WatchCoordinator::refresh()
{
  const auto listed = input_.camera->listWatch();
  if (!listed)
    return;
  std::map<int64_t, WatchCameraInfo> cameras;
  std::vector<int64_t> listening;
  for (const auto& camera : *listed) {
    cameras[camera.camera_id()] = {.id = camera.camera_id(),
                                   .name = camera.name(),
                                   .talk = camera.talk(),
                                   .panTilt = camera.pan_tilt(),
                                   .tvExclusion = camera.tv_exclusion()};
    if (camera.listening())
      listening.push_back(camera.camera_id());
  }
  std::vector<std::unique_ptr<CameraEar>> retired;
  std::vector<CameraEar*> started;
  {
    std::scoped_lock lock(mutex_);
    cameras_ = std::move(cameras);
    for (auto it = ears_.begin(); it != ears_.end();) {
      if (std::ranges::find(listening, it->first) == listening.end()) {
        retired.push_back(std::move(it->second));
        it = ears_.erase(it);
      }
      else {
        ++it;
      }
    }
    for (const int64_t cameraId : listening) {
      if (ears_.contains(cameraId))
        continue;
      auto ear = std::make_unique<CameraEar>(CameraEarInput{
          .cameraId = cameraId,
          .name = cameras_[cameraId].name,
          .camera = input_.camera,
          .spotter = input_.spotter,
          .vad = input_.vad,
          .stt = input_.stt,
          .language = input_.language,
          .config = input_.config,
          .onFire = [this](EarFire fire) { enqueue(std::move(fire)); },
          .onFrame = input_.onFrame,
          .playing = [echo = input_.echo](int64_t id) {
            return echo && echo->muted(id, WatchClock::now());
          }});
      started.push_back(ear.get());
      ears_.emplace(cameraId, std::move(ear));
      LOG_INFO << "Voice watch: camera " << cameraId << " (" << cameras_[cameraId].name << ") listens for Argus";
    }
  }
  for (CameraEar* ear : started)
    ear->start();
  for (const auto& ear : retired)
    LOG_INFO << "Voice watch: camera " << ear->cameraId() << " stops listening";
  retired.clear();
}

void WatchCoordinator::enqueue(EarFire fire)
{
  {
    std::scoped_lock lock(queueMutex_);
    queue_.push_back(std::move(fire));
  }
  queueReady_.notify_all();
}

void WatchCoordinator::workerLoop(const std::stop_token& stop)
{
  while (!stop.stop_requested()) {
    std::vector<EarFire> window;
    {
      std::unique_lock lock(queueMutex_);
      if (!queueReady_.wait(lock, stop, [this] { return !queue_.empty(); }))
        return;
      window.push_back(std::move(queue_.front()));
      queue_.pop_front();
      const auto closeAt = window.front().at + input_.config.broadcastWindow;
      static_cast<void>(queueReady_.wait_until(lock, stop, closeAt, [] { return false; }));
      while (!queue_.empty() && queue_.front().at <= closeAt) {
        window.push_back(std::move(queue_.front()));
        queue_.pop_front();
      }
    }
    if (stop.stop_requested())
      return;
    decide(std::move(window));
  }
}

void WatchCoordinator::decide(std::vector<EarFire> fires)
{
  std::map<int64_t, WatchCameraInfo> cameras;
  {
    std::scoped_lock lock(mutex_);
    cameras = cameras_;
  }
  const WakeDecision decision = decideWakes({.fires = fires, .cameras = cameras, .config = input_.config});
  if (decision.broadcast) {
    for (const EarFire& fire : fires)
      record(fire, WakeVerdict::Broadcast, nullptr);
    return;
  }
  for (const size_t index : decision.quiet)
    record(fires[index], WakeVerdict::LowSnr, nullptr);
  for (const size_t index : decision.duplicates)
    record(fires[index], WakeVerdict::DuplicateWake, nullptr);
  for (const WakeCandidate& candidate : decision.candidates)
    confirm(fires[candidate.fire], candidate.threshold);
}

void WatchCoordinator::confirm(const EarFire& fire, float threshold)
{
  if (fire.duringPlayback) {
    record(fire, WakeVerdict::SelfEcho, nullptr);
    return;
  }
  int maxDistance = input_.config.wakeMaxDistance;
  if (input_.echo) {
    const EchoQuery query{.cameraId = fire.cameraId,
                          .micEnvelope = fire.envelope,
                          .micStart = fire.envelopeStart,
                          .sameEnvironment = input_.sameEnvironment};
    if (input_.echo->othersPlaying(query, fire.at)) {
      if (fire.snrDb < threshold + input_.config.echoRaiseDb ||
          input_.echo->correlationWithOthers(query) >= input_.config.echoCorrelation) {
        record(fire, WakeVerdict::CrossEcho, nullptr);
        return;
      }
      maxDistance = std::max(0, maxDistance - 1);
    }
  }
  StageTwoResult result;
  if (fire.transcript) {
    result = judgeWake(*fire.transcript, maxDistance);
  }
  else {
    static_cast<void>(fire.capture->wait(input_.config.postRoll + kStageTwoGrace));
    const std::vector<float> samples = fire.capture->snapshot();
    result = stageTwo_.confirm({.samples = samples, .language = input_.language, .maxDistance = maxDistance});
  }
  if (!result.accepted) {
    record(fire, WakeVerdict::Rejected, &result);
    return;
  }
  record(fire, WakeVerdict::Accepted, &result);
  if (input_.onWake) {
    std::string name;
    if (const auto info = camera(fire.cameraId))
      name = info->name;
    input_.onWake({.cameraId = fire.cameraId,
                   .cameraName = name,
                   .stageTwo = result,
                   .capture = fire.capture,
                   .snrDb = fire.snrDb,
                   .at = fire.at});
  }
}

void WatchCoordinator::record(const EarFire& fire, WakeVerdict verdict, const StageTwoResult* stageTwo)
{
  fireLog_.record({.cameraId = fire.cameraId,
                   .at = std::chrono::system_clock::now(),
                   .verdict = verdict,
                   .source = fire.source,
                   .snrDb = fire.snrDb,
                   .distance = stageTwo != nullptr ? stageTwo->match.distance : -1,
                   .stageTwo = stageTwo != nullptr ? stageTwo->confidence : 0.0F,
                   .score = fire.snrDb});
}
