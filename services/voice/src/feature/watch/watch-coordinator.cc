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

float wakeScore(const WakeScoreInput& input, const VoiceWatchConfig& config)
{
  return (config.weightWake * input.wake) + (config.weightSnr * input.normalizedSnr) +
         (config.weightStageTwo * input.stageTwo);
}

WakeDecision decideWakes(const WakeDecisionInput& input)
{
  WakeDecision decision;
  if (broadcastBurst(input)) {
    decision.broadcast = true;
    return decision;
  }
  std::vector<WakeCandidate> passing;
  for (size_t index = 0; index < input.fires.size(); ++index) {
    const EarFire& fire = input.fires[index];
    const auto camera = input.cameras.find(fire.cameraId);
    const bool tv = camera != input.cameras.end() && camera->second.tvExclusion;
    const float threshold = input.config.minWakeSnrDb + (tv ? input.config.tvRaiseDb : 0.0F);
    if (fire.snrDb < threshold) {
      decision.quiet.push_back(index);
      continue;
    }
    const float normalized = input.normalize ? input.normalize(fire.cameraId, fire.snrDb) : fire.snrDb;
    passing.push_back({.fire = index, .threshold = threshold, .normalizedSnr = normalized});
  }
  std::ranges::stable_sort(passing, std::ranges::greater{}, &WakeCandidate::normalizedSnr);
  std::vector<int64_t> seen;
  for (const WakeCandidate& candidate : passing) {
    const int64_t cameraId = input.fires[candidate.fire].cameraId;
    const bool repeat = std::ranges::find(seen, cameraId) != seen.end();
    if (!repeat && std::cmp_less(decision.candidates.size(), input.config.stageTwoCandidates)) {
      decision.candidates.push_back(candidate);
      seen.push_back(cameraId);
    }
    else {
      decision.duplicates.push_back(candidate.fire);
    }
  }
  return decision;
}

WatchCoordinator::WatchCoordinator(WatchCoordinatorInput input)
    : input_(std::move(input)),
      stageTwo_(*input_.stt),
      fireLog_(static_cast<size_t>(input_.config.fireLogSize)),
      gains_(input_.config.gainAlpha)
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
  out.reserve(cameras_.size());
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

bool WatchCoordinator::listening(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = ears_.find(cameraId);
  return found != ears_.end() && found->second->listening();
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
  if (input_.guard && input_.rooms) {
    if (const auto rooms = input_.guard->cameraRooms()) {
      std::vector<CameraPlace> places;
      places.reserve(rooms->size());
      for (const auto& room : *rooms)
        places.push_back({.cameraId = room.camera_id(),
                          .environmentId = room.environment_id(),
                          .room = room.room(),
                          .outdoor = room.outdoor()});
      input_.rooms->replace(places);
    }
  }
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
      const auto closeAt = window.front().at + input_.config.arbitrationWindow;
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

bool WatchCoordinator::sameEnvironment(int64_t left, int64_t right) const
{
  return !input_.rooms || input_.rooms->sameEnvironment(left, right);
}

void WatchCoordinator::decide(std::vector<EarFire> fires)
{
  std::map<int64_t, WatchCameraInfo> cameras;
  {
    std::scoped_lock lock(mutex_);
    cameras = cameras_;
  }
  const WakeDecision decision = decideWakes(
      {.fires = fires,
       .cameras = cameras,
       .config = input_.config,
       .normalize = [this](int64_t cameraId, float snr) {
         return gains_.normalize(cameraId, snr, input_.config.gainWeight);
       }});
  if (decision.broadcast) {
    for (const EarFire& fire : fires)
      record(fire, WakeVerdict::Broadcast, nullptr);
    return;
  }
  for (const size_t index : decision.quiet)
    record(fires[index], WakeVerdict::LowSnr, nullptr);
  for (const size_t index : decision.duplicates)
    record(fires[index], WakeVerdict::DuplicateWake, nullptr);
  if (decision.candidates.empty())
    return;

  std::vector<Evaluation> evaluations(decision.candidates.size());
  {
    std::vector<std::jthread> jobs;
    jobs.reserve(decision.candidates.size());
    for (size_t index = 0; index < decision.candidates.size(); ++index)
      jobs.emplace_back([this, &fires, &decision, &evaluations, index] {
        evaluations[index] = evaluate(fires[decision.candidates[index].fire], decision.candidates[index]);
      });
  }
  std::optional<size_t> winner;
  for (size_t index = 0; index < evaluations.size(); ++index) {
    if (evaluations[index].verdict != WakeVerdict::Accepted)
      continue;
    if (!winner || evaluations[index].score > evaluations[*winner].score)
      winner = index;
  }
  for (size_t index = 0; index < evaluations.size(); ++index) {
    const EarFire& fire = fires[decision.candidates[index].fire];
    if (winner && index != *winner && evaluations[index].verdict == WakeVerdict::Accepted)
      record(fire, WakeVerdict::DuplicateWake, &evaluations[index]);
    else
      record(fire, evaluations[index].verdict, &evaluations[index]);
  }
  if (!winner)
    return;
  const EarFire& fire = fires[decision.candidates[*winner].fire];
  gains_.observe(fire.cameraId, fire.snrDb);
  if (input_.onWake) {
    std::string name;
    if (const auto info = camera(fire.cameraId))
      name = info->name;
    input_.onWake({.cameraId = fire.cameraId,
                   .cameraName = name,
                   .stageTwo = evaluations[*winner].stageTwo,
                   .capture = fire.capture,
                   .snrDb = fire.snrDb,
                   .at = fire.at});
  }
}

WatchCoordinator::Evaluation WatchCoordinator::evaluate(const EarFire& fire, const WakeCandidate& candidate)
{
  Evaluation out;
  if (fire.duringPlayback) {
    out.verdict = input_.onPlaybackFire && input_.onPlaybackFire(fire) ? WakeVerdict::BargeIn : WakeVerdict::SelfEcho;
    return out;
  }
  int maxDistance = input_.config.wakeMaxDistance;
  if (input_.echo) {
    const EchoQuery query{.cameraId = fire.cameraId,
                          .micEnvelope = fire.envelope,
                          .micStart = fire.envelopeStart,
                          .sameEnvironment = [this](int64_t left, int64_t right) {
                            return sameEnvironment(left, right);
                          }};
    if (input_.echo->othersPlaying(query, fire.at)) {
      if (fire.snrDb < candidate.threshold + input_.config.echoRaiseDb ||
          input_.echo->correlationWithOthers(query) >= input_.config.echoCorrelation) {
        out.verdict = WakeVerdict::CrossEcho;
        return out;
      }
      maxDistance = std::max(0, maxDistance - 1);
    }
  }
  if (fire.transcript) {
    out.stageTwo = judgeWake(*fire.transcript, maxDistance);
  }
  else if (fire.capture) {
    static_cast<void>(fire.capture->wait(input_.config.postRoll + kStageTwoGrace));
    const std::vector<float> samples = fire.capture->snapshot();
    out.stageTwo = stageTwo_.confirm({.samples = samples, .language = input_.language, .maxDistance = maxDistance});
  }
  out.verdict = out.stageTwo.accepted ? WakeVerdict::Accepted : WakeVerdict::Rejected;
  out.score = wakeScore({.wake = 1.0F, .normalizedSnr = candidate.normalizedSnr, .stageTwo = out.stageTwo.confidence},
                        input_.config);
  return out;
}

void WatchCoordinator::record(const EarFire& fire, WakeVerdict verdict, const Evaluation* evaluation)
{
  fireLog_.record({.cameraId = fire.cameraId,
                   .at = std::chrono::system_clock::now(),
                   .verdict = verdict,
                   .source = fire.source,
                   .snrDb = fire.snrDb,
                   .distance = evaluation != nullptr ? evaluation->stageTwo.match.distance : -1,
                   .stageTwo = evaluation != nullptr ? evaluation->stageTwo.confidence : 0.0F,
                   .score = evaluation != nullptr ? evaluation->score : 0.0F});
}
