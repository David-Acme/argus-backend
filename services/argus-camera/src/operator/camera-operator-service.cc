#include <operator/camera-operator-service.hxx>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/base64/base64.hxx>
#include <shared/services/evidence/evidence-uploader.hxx>
#include <shared/services/stream/snapshot-store.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <thread>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int severityRank(const std::string& severity)
{
  if (severity == "critical")
    return 2;
  if (severity == "warning")
    return 1;
  return 0;
}

std::string severityName(EventSeverity severity)
{
  switch (severity) {
    case EventSeverity::Critical:
      return "critical";
    case EventSeverity::Warning:
      return "warning";
    case EventSeverity::Info:
      return "info";
  }
  return "info";
}

// Compact Lab color histogram (8 bins per channel) for cross-camera matching.
std::string personSignature(const cv::Mat& rgb, const DetectedObject& object)
{
  const int x = std::clamp(static_cast<int>(object.x), 0,
                           std::max(0, rgb.cols - 1));
  const int y = std::clamp(static_cast<int>(object.y), 0,
                           std::max(0, rgb.rows - 1));
  const int w = std::clamp(static_cast<int>(object.w), 1, rgb.cols - x);
  const int h = std::clamp(static_cast<int>(object.h), 1, rgb.rows - y);
  if (w < 8 || h < 8)
    return {};

  cv::Mat bgr;
  cv::cvtColor(rgb(cv::Rect(x, y, w, h)), bgr, cv::COLOR_RGB2BGR);
  cv::Mat lab;
  cv::cvtColor(bgr, lab, cv::COLOR_BGR2Lab);

  std::array<int, 24> bins{};
  for (int row = 0; row < lab.rows; row += 2) {
    const uint8_t* pixel = lab.ptr<uint8_t>(row);
    for (int column = 0; column < lab.cols; column += 2) {
      const uint8_t* value = pixel + column * 3;
      ++bins[value[0] >> 5];
      ++bins[8 + (value[1] >> 5)];
      ++bins[16 + (value[2] >> 5)];
    }
  }

  const int peak =
      *std::max_element(bins.begin(), bins.end());
  if (peak <= 0)
    return {};
  std::string quantized(24, '\0');
  for (int i = 0; i < 24; ++i)
    quantized[i] = static_cast<char>(bins[i] * 255 / peak);
  return base64::encode(quantized);
}
} // namespace

CameraOperatorService::CameraOperatorService(Inputs inputs)
    : inputs_(std::move(inputs))
{
}

void CameraOperatorService::start()
{
  if (running_.exchange(true))
    return;

  rescan();

  drogon::async_run([this]() -> drogon::Task<void> {
    co_await supervise();
  });
}

void CameraOperatorService::stop()
{
  running_.store(false);
  std::lock_guard<std::mutex> lock(camerasMutex_);
  for (auto& [id, stop] : cameraStop_)
    stop->store(true);
  cameraStop_.clear();
}

void CameraOperatorService::rescan()
{
  std::vector<CameraRef> cameras;
  try {
    const auto rows = DbService::client()->execSqlSync(
        "SELECT id, name FROM camera "
        "WHERE deleted_at IS NULL AND is_enabled = 1");
    for (const auto& row : rows)
      cameras.push_back({row["id"].as<int64_t>(), row["name"].as<std::string>()});
  }
  catch (const std::exception& e) {
    LOG_WARN << "Camera operator: camera discovery failed ("
             << e.what() << "); keeping current loops";
    return;
  }

  std::lock_guard<std::mutex> lock(camerasMutex_);
  std::vector<int64_t> removed;
  for (const auto& [id, stop] : cameraStop_) {
    const bool present =
        std::any_of(cameras.begin(), cameras.end(),
                    [id](const CameraRef& camera) { return camera.id == id; });
    if (!present) {
      stop->store(true);
      removed.push_back(id);
    }
  }
  for (const int64_t id : removed)
    cameraStop_.erase(id);

  std::vector<CameraRef> added;
  for (const auto& camera : cameras) {
    if (cameraStop_.contains(camera.id))
      continue;
    auto stop = std::make_shared<std::atomic<bool>>(false);
    cameraStop_[camera.id] = stop;
    added.push_back(camera);
    drogon::async_run([this, camera, stop]() -> drogon::Task<void> {
      co_await runCamera(camera, stop);
    });
  }

  if (!added.empty() || !removed.empty()) {
    LOG_INFO << "Camera operator: " << added.size()
             << " camera loop(s) added, " << removed.size() << " removed";
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    for (const int64_t id : removed)
      states_.erase(id);
  }
}

drogon::Task<void> CameraOperatorService::supervise()
{
  while (running_.load()) {
    co_await BlockingTask<void>{[this]() {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(inputs_.operator_.cameraRescanMs));
    }};
    if (!running_.load())
      break;
    co_await BlockingTask<void>{[this]() { rescan(); }};
  }
  co_return;
}

bool CameraOperatorService::isNightHour(int hour) const
{
  const int start = inputs_.operator_.nightStartHour;
  const int end = inputs_.operator_.nightEndHour;
  if (start == end)
    return false;
  return start < end ? (hour >= start && hour < end)
                     : (hour >= start || hour < end);
}

bool CameraOperatorService::cameraHasMotion(const cv::Mat& rgb,
                                            CameraState& state) const
{
  cv::Mat gray;
  cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);
  cv::Mat small;
  cv::resize(gray, small, cv::Size(160, 90), 0, 0, cv::INTER_AREA);

  const size_t pixels = small.total();
  if (state.motionGray.size() != pixels) {
    state.motionGray.assign(small.data, small.data + pixels);
    return true;
  }

  int changed = 0;
  for (int row = 0; row < small.rows; ++row) {
    const uint8_t* current = small.ptr<uint8_t>(row);
    const uint8_t* previous =
        state.motionGray.data() + static_cast<size_t>(row) * small.cols;
    for (int column = 0; column < small.cols; ++column) {
      if (std::abs(static_cast<int>(current[column]) -
                   static_cast<int>(previous[column])) > 25)
        ++changed;
    }
  }
  state.motionGray.assign(small.data, small.data + pixels);
  return changed >= static_cast<int>(
                        inputs_.operator_.motionMinRatio *
                        static_cast<double>(pixels));
}

std::vector<bool> CameraOperatorService::staticBoxMask(
    CameraState& state, const std::vector<DetectedObject>& objects) const
{
  struct Assignment
  {
    size_t objectIndex{0};
    size_t trackIndex{0};
    float iou{0.0F};
  };

  std::vector<Assignment> pairs;
  std::map<std::string, std::vector<bool>> used;
  for (size_t index = 0; index < objects.size(); ++index) {
    auto& tracks = state.boxTracks[objects[index].name];
    if (used.find(objects[index].name) == used.end())
      used[objects[index].name] = std::vector<bool>(tracks.size(), false);
    for (size_t track = 0; track < tracks.size(); ++track) {
      const BoxTrack& candidate = tracks[track];
      const float x1 = std::max(candidate.x, objects[index].x);
      const float y1 = std::max(candidate.y, objects[index].y);
      const float x2 = std::min(candidate.x + candidate.w,
                                objects[index].x + objects[index].w);
      const float y2 = std::min(candidate.y + candidate.h,
                                objects[index].y + objects[index].h);
      const float intersection =
          std::max(0.0F, x2 - x1) * std::max(0.0F, y2 - y1);
      const float total =
          candidate.w * candidate.h + objects[index].w * objects[index].h -
          intersection;
      pairs.push_back({.objectIndex = index,
                       .trackIndex = track,
                       .iou = total > 0.0F ? intersection / total : 0.0F});
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const Assignment& left,
                                           const Assignment& right) {
    return left.iou > right.iou;
  });

  std::vector<int> trackForObject(objects.size(), -1);
  for (const auto& pair : pairs) {
    if (pair.iou <= 0.0F || trackForObject[pair.objectIndex] >= 0)
      continue;
    const std::string& name = objects[pair.objectIndex].name;
    auto& tracks = state.boxTracks[name];
    if (pair.trackIndex >= tracks.size() || used[name][pair.trackIndex])
      continue;
    used[name][pair.trackIndex] = true;
    trackForObject[pair.objectIndex] = static_cast<int>(pair.trackIndex);
    tracks[pair.trackIndex].frames += 1;
  }

  std::vector<bool> result(objects.size(), false);
  for (size_t index = 0; index < objects.size(); ++index) {
    auto& tracks = state.boxTracks[objects[index].name];
    if (trackForObject[index] < 0) {
      BoxTrack track;
      track.x = objects[index].x;
      track.y = objects[index].y;
      track.w = objects[index].w;
      track.h = objects[index].h;
      track.frames = 1;
      tracks.push_back(track);
      result[index] =
          track.frames >= inputs_.operator_.staticBoxFrames;
      continue;
    }
    BoxTrack& track = tracks[trackForObject[index]];
    const bool stationary =
        std::abs(track.x - objects[index].x) < 3.0F &&
        std::abs(track.y - objects[index].y) < 3.0F &&
        std::abs(track.w - objects[index].w) < 3.0F &&
        std::abs(track.h - objects[index].h) < 3.0F;
    if (!stationary)
      track.frames = 1;
    track.x = objects[index].x;
    track.y = objects[index].y;
    track.w = objects[index].w;
    track.h = objects[index].h;
    result[index] = track.frames >= inputs_.operator_.staticBoxFrames;
  }
  for (auto& [name, tracks] : state.boxTracks) {
    if (tracks.size() > 32)
      tracks.erase(tracks.begin(), tracks.begin() + (tracks.size() - 32));
  }
  return result;
}

CameraOperatorService::PersonDwell CameraOperatorService::updatePersonTracks(
    const PersonTrackInput& input) const
{
  CameraState& state = input.state;
  std::vector<DetectedObject>& objects = input.objects;
  const std::vector<OperatorZone>& zones = input.zones;
  const int64_t stamp = input.stamp;
  const bool night = input.night;
  const int frameWidth = input.frameWidth;
  const int frameHeight = input.frameHeight;

  const auto iou = [](const PersonTrack& track, const DetectedObject& object) {
    const float x1 = std::max(track.x, object.x);
    const float y1 = std::max(track.y, object.y);
    const float x2 = std::min(track.x + track.w, object.x + object.w);
    const float y2 = std::min(track.y + track.h, object.y + object.h);
    const float intersection =
        std::max(0.0F, x2 - x1) * std::max(0.0F, y2 - y1);
    const float total = track.w * track.h + object.w * object.h - intersection;
    return total > 0.0F ? intersection / total : 0.0F;
  };

  for (auto it = state.personTracks.begin(); it != state.personTracks.end();) {
    if (stamp - it->second.lastSeenMs > inputs_.operator_.trackTtlMs)
      it = state.personTracks.erase(it);
    else
      ++it;
  }

  struct Assignment
  {
    size_t objectIndex{0};
    int64_t trackId{0};
    float iou{0.0F};
  };

  std::vector<Assignment> pairs;
  for (size_t index = 0; index < objects.size(); ++index) {
    if (objects[index].name != "person")
      continue;
    for (const auto& [trackId, track] : state.personTracks)
      pairs.push_back({.objectIndex = index,
                       .trackId = trackId,
                       .iou = iou(track, objects[index])});
  }
  std::sort(pairs.begin(), pairs.end(), [](const Assignment& left,
                                           const Assignment& right) {
    return left.iou > right.iou;
  });

  std::vector<int64_t> trackForObject(objects.size(), 0);
  std::vector<bool> trackUsed(objects.size(), false);
  std::vector<int64_t> reserved;
  reserved.reserve(state.personTracks.size());
  const auto isReserved = [&reserved](int64_t id) {
    return std::find(reserved.begin(), reserved.end(), id) != reserved.end();
  };

  for (const auto& pair : pairs) {
    if (pair.iou < static_cast<float>(inputs_.operator_.trackIouMin) ||
        trackForObject[pair.objectIndex] != 0 || isReserved(pair.trackId))
      continue;
    trackForObject[pair.objectIndex] = pair.trackId;
    reserved.push_back(pair.trackId);
  }

  PersonDwell dwell;
  for (size_t index = 0; index < objects.size(); ++index) {
    if (objects[index].name != "person")
      continue;
    int64_t trackId = trackForObject[index];
    if (trackId == 0) {
      PersonTrack track;
      track.id = state.nextPersonTrackId++;
      track.firstSeenMs = stamp;
      state.personTracks[track.id] = track;
      trackId = track.id;
      trackForObject[index] = trackId;
    }
    PersonTrack& track = state.personTracks[trackId];
    const bool stationary = std::abs(track.x - objects[index].x) < 3.0F &&
                            std::abs(track.y - objects[index].y) < 3.0F &&
                            std::abs(track.w - objects[index].w) < 3.0F &&
                            std::abs(track.h - objects[index].h) < 3.0F;
    track.staticFrames = stationary ? track.staticFrames + 1 : 0;
    track.x = objects[index].x;
    track.y = objects[index].y;
    track.w = objects[index].w;
    track.h = objects[index].h;
    track.lastSeenMs = stamp;
    objects[index].trackId = trackId;
    objects[index].firstSeenMs = track.firstSeenMs;
    const bool inAlertZone = std::any_of(
        zones.begin(), zones.end(), [&](const OperatorZone& zone) {
          return zone.kind == "alert" &&
                 objectCenterInZone({.object = objects[index],
                                     .zone = zone,
                                     .frameWidth = frameWidth,
                                     .frameHeight = frameHeight});
        });
    const int64_t threshold =
        inAlertZone ? inputs_.operator_.dwellAlertMs
                    : (night ? inputs_.operator_.dwellNightMs
                             : inputs_.operator_.dwellMonitorMs);
    const int64_t dwellMs = stamp - track.firstSeenMs;
    const bool due = dwellMs >= threshold;
    const bool recheckDue =
        track.lastEmitMs == 0 ||
        stamp - track.lastEmitMs >= inputs_.operator_.personRecheckMs;
    const bool canEmit = due && recheckDue;
    dwell.tracks.push_back({.trackId = trackId,
                            .dwellMs = dwellMs,
                            .confidence = objects[index].confidence,
                            .inAlertZone = inAlertZone,
                            .due = due,
                            .canEmit = canEmit});
    dwell.personPresent = true;
    if (due)
      dwell.dwelling = true;
  }
  return dwell;
}

double CameraOperatorService::currentInferenceFps(int64_t cameraId)
{
  const int64_t stamp = nowMs();
  std::lock_guard<std::mutex> lock(stateMutex_);
  const auto found = states_.find(cameraId);
  if (found == states_.end())
    return inputs_.objects.maxFpsInference;
  const CameraState& state = found->second;
  if (stamp < state.burstUntilMs)
    return inputs_.objects.burstFps;
  if (state.presenceStreak > 0 || !state.pendingPersons.empty() ||
      state.pendingOther.has_value() || !state.personTracks.empty())
    return inputs_.objects.activeFps;
  return inputs_.objects.maxFpsInference;
}

drogon::Task<void> CameraOperatorService::runCamera(
    CameraRef camera, std::shared_ptr<std::atomic<bool>> stop)
{
  while (running_.load() && !stop->load()) {
    const int64_t tickStart = nowMs();
    const int64_t intervalMs =
        static_cast<int64_t>(1000.0 / currentInferenceFps(camera.id));
    auto frame = co_await inputs_.dependencies.source->grab(
        {.cameraId = camera.id, .cameraName = camera.name});
    if (frame) {
      co_await BlockingTask<void>{[this, &camera, &frame]() {
        processFrame({.cameraId = camera.id, .cameraName = camera.name,
                      .frame = *frame});
      }};
    }
    const int64_t elapsed = nowMs() - tickStart;
    const int64_t remaining = std::max<int64_t>(0, intervalMs - elapsed);
    if (remaining > 0)
      co_await BlockingTask<void>{[remaining]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(remaining));
      }};
  }
  co_return;
}

void CameraOperatorService::processFrame(const ProcessFrameInput& input)
{
  const int64_t cameraId = input.cameraId;
  const std::string& cameraName = input.cameraName;
  CameraFrame& frame = input.frame;
  if (!inputs_.dependencies.detector ||
      !inputs_.dependencies.detector->isLoaded())
    return;

  cv::Mat rgb;
  if (!frame.jpeg.empty()) {
    const cv::Mat raw =
        cv::imdecode(frame.jpeg, cv::IMREAD_COLOR);
    if (raw.empty()) {
      LOG_WARN << "Camera operator: undecodable frame for camera " << cameraId;
      return;
    }
    cv::cvtColor(raw, rgb, cv::COLOR_BGR2RGB);
  } else if (!frame.rgb.empty()) {
    rgb = cv::Mat(frame.height, frame.width, CV_8UC3,
                  const_cast<uint8_t*>(frame.rgb.data()));
  } else {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    CameraState& gateState = states_[cameraId];
    if (inputs_.operator_.motionGate && gateState.presenceStreak == 0 &&
        gateState.pendingPersons.empty() && !gateState.pendingOther &&
        !cameraHasMotion(rgb, gateState))
      return;
  }

  auto objects = inputs_.dependencies.detector->detect(
      {.rgb = reinterpret_cast<const uint8_t*>(rgb.data),
       .width = rgb.cols,
       .height = rgb.rows});

  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (inputs_.operator_.ignoreStaticPersons) {
      const std::vector<bool> mask = staticBoxMask(states_[cameraId], objects);
      std::vector<DetectedObject> kept;
      kept.reserve(objects.size());
      for (size_t index = 0; index < objects.size(); ++index) {
        if (objects[index].name == "person" && mask[index])
          continue;
        kept.push_back(objects[index]);
      }
      objects = std::move(kept);
    }
  }

  if (inputs_.operator_.overlay && !inputs_.operator_.overlayDir.empty()) {
    cv::Mat annotated;
    cv::cvtColor(rgb, annotated, cv::COLOR_RGB2BGR);
    for (const auto& object : objects) {
      cv::rectangle(annotated,
                    cv::Rect(static_cast<int>(object.x), static_cast<int>(object.y),
                             static_cast<int>(object.w), static_cast<int>(object.h)),
                    cv::Scalar(0, 255, 0), 2);
    }
    std::filesystem::create_directories(inputs_.operator_.overlayDir);
    cv::imwrite(inputs_.operator_.overlayDir + "/cam" +
                    std::to_string(cameraId) + ".jpg",
                annotated);
  }

  const std::time_t tick = std::time(nullptr);
  int hour = 0;
  std::tm local{};
  if (localtime_r(&tick, &local))
    hour = local.tm_hour;

  const int64_t stamp = nowMs();
  if (!frame.jpeg.empty()) {
    SnapshotStore::instance().putFrame(
        cameraId, std::string(frame.jpeg.begin(), frame.jpeg.end()), stamp);
  }
  std::lock_guard<std::mutex> lock(stateMutex_);
  CameraState& state = states_[cameraId];

  const bool hasPerson =
      std::any_of(objects.begin(), objects.end(),
                  [](const DetectedObject& object) {
                    return object.name == "person";
                  });
  const bool hasVehicle =
      std::any_of(objects.begin(), objects.end(),
                  [](const DetectedObject& object) {
                    return object.name == "car" || object.name == "truck" ||
                           object.name == "bus" || object.name == "motorcycle";
                  });

  OperatorState operatorState;
  operatorState.vehiclePreviouslyAbsent =
      state.vehicleLastSeenMs != 0 &&
      stamp - state.vehicleLastSeenMs >= inputs_.operator_.cooldownMs;
  state.presenceStreak =
      (hasPerson || hasVehicle) ? state.presenceStreak + 1 : 0;
  operatorState.presenceEscalating =
      state.presenceStreak >= inputs_.operator_.presenceEscalationFrames;

  EventIntelligenceInput intelligence;
  intelligence.cameraId = cameraId;
  if (inputs_.dependencies.zones)
    intelligence.zones = inputs_.dependencies.zones->forCamera(cameraId);
  else
    for (const auto& zone : inputs_.operator_.zones) {
      if (zone.cameraId == cameraId)
        intelligence.zones.push_back(zone);
    }
  intelligence.ignoredClasses = inputs_.operator_.ignoredClasses;
  const bool night = isNightHour(hour);
  const PersonDwell dwell = updatePersonTracks(
      {.state = state,
       .objects = objects,
       .zones = intelligence.zones,
       .stamp = stamp,
       .night = night,
       .frameWidth = rgb.cols,
       .frameHeight = rgb.rows});
  for (const auto& object : objects) {
    if (object.name != "person" || object.trackId <= 0)
      continue;
    const auto found = state.personTracks.find(object.trackId);
    if (found == state.personTracks.end())
      continue;
    const double area = static_cast<double>(object.w) * object.h;
    if (area >= found->second.bestScore) {
      found->second.bestScore = area;
      found->second.signature = personSignature(rgb, object);
    }
  }
  intelligence.objects = objects;
  intelligence.night = night;
  intelligence.state = operatorState;
  if (dwell.personPresent)
    state.burstUntilMs = stamp + inputs_.objects.burstMs;
  intelligence.matcher = inputs_.dependencies.matcher;
  intelligence.frameRgb = rgb.data;
  intelligence.frameWidth = rgb.cols;
  intelligence.frameHeight = rgb.rows;

  int emittedTracks = 0;
  for (const auto& verdict : dwell.tracks) {
    if (!verdict.canEmit)
      continue;
    intelligence.persons.clear();
    intelligence.persons.push_back({.trackId = verdict.trackId,
                                    .due = verdict.due,
                                    .canEmit = verdict.canEmit});
    intelligence.primaryTrackId = verdict.trackId;
    const EventIntelligenceOutcome outcome =
        EventIntelligence::evaluate(intelligence);
    if (!outcome.publish)
      continue;
    mergePersonPending({.cameraId = cameraId,
                        .cameraName = cameraName,
                        .state = state,
                        .outcome = outcome,
                        .objects = objects,
                        .stamp = stamp,
                        .frameWidth = rgb.cols,
                        .frameHeight = rgb.rows,
                        .trackId = verdict.trackId,
                        .dwellMs = verdict.dwellMs});
    ++emittedTracks;
  }

  if (hasVehicle && emittedTracks == 0 && !state.pendingOther) {
    intelligence.persons.clear();
    intelligence.primaryTrackId = 0;
    const EventIntelligenceOutcome outcome =
        EventIntelligence::evaluate(intelligence);
    if (outcome.publish) {
      state.pendingOther = ObjectDetectedEvent{};
      state.pendingOtherStartMs = stamp;
      state.pendingOther->cameraId = cameraId;
      state.pendingOther->cameraName = cameraName;
      state.pendingOther->capturedAtMs = stamp;
      state.pendingOther->frameWidth = rgb.cols;
      state.pendingOther->frameHeight = rgb.rows;
      state.pendingOther->rule = outcome.rule;
      state.pendingOther->severity = severityName(outcome.severity);
      state.pendingOther->escalated = outcome.escalated;
      state.pendingOther->knownPersonId = outcome.knownPersonId;
      for (const auto& evaluated : outcome.objects)
        mergeObject({.event = *state.pendingOther,
                     .evaluated = evaluated,
                     .state = state,
                     .stamp = stamp});
    }
  }
  else if (state.pendingOther) {
    for (const auto& object : objects) {
      mergeObject({.event = *state.pendingOther,
                   .evaluated = {.object = object,
                                 .personId = std::nullopt,
                                 .known = false,
                                 .identityConfidence = 0.0F,
                                 .zoneKind = {}},
                   .state = state,
                   .stamp = stamp});
    }
  }

  if (hasVehicle)
    state.vehicleLastSeenMs = stamp;

  std::vector<int64_t> dueTracks;
  for (const auto& [trackId, event] : state.pendingPersons) {
    const auto start = state.pendingPersonStartMs.find(trackId);
    if (start != state.pendingPersonStartMs.end() &&
        stamp - start->second >= inputs_.operator_.aggregationWindowMs)
      dueTracks.push_back(trackId);
  }
  for (const int64_t trackId : dueTracks)
    publishPersonPending({.cameraId = cameraId,
                          .state = state,
                          .trackId = trackId,
                          .nowMs = stamp});

  if (state.pendingOther &&
      stamp - state.pendingOtherStartMs >= inputs_.operator_.aggregationWindowMs)
    publishOtherPending({.cameraId = cameraId, .state = state, .nowMs = stamp});
}

void CameraOperatorService::mergeObject(const MergeObjectInput& input)
{
  ObjectDetectedEvent& event = input.event;
  const DetectedObject& object = input.evaluated.object;
  const bool person = object.name == "person";
  const auto existing = std::find_if(
      event.objects.begin(), event.objects.end(),
      [&](const DetectedEventObject& entry) {
        if (person && object.trackId > 0)
          return entry.trackId == object.trackId;
        return entry.name == object.name;
      });
  const PersonTrack* track = nullptr;
  if (person && object.trackId > 0) {
    const auto found = input.state.personTracks.find(object.trackId);
    if (found != input.state.personTracks.end())
      track = &found->second;
  }
  if (existing != event.objects.end()) {
    if (existing->personId == 0 && input.evaluated.personId &&
        *input.evaluated.personId > 0) {
      existing->personId = *input.evaluated.personId;
      existing->identity = input.evaluated.known ? "known" : "unknown";
    }
    existing->identityConfidence = input.evaluated.identityConfidence;
    existing->confidence = std::max(existing->confidence, object.confidence);
    existing->x = object.x;
    existing->y = object.y;
    existing->w = object.w;
    existing->h = object.h;
    if (track) {
      existing->lastSeenMs = track->lastSeenMs;
      existing->dwellMs = input.stamp - track->firstSeenMs;
      if (!track->signature.empty())
        existing->signature = track->signature;
    }
    if (!input.evaluated.zoneKind.empty())
      existing->zoneKind = input.evaluated.zoneKind;
    return;
  }

  DetectedEventObject entry;
  entry.name = object.name;
  entry.confidence = object.confidence;
  entry.x = object.x;
  entry.y = object.y;
  entry.w = object.w;
  entry.h = object.h;
  entry.personId = input.evaluated.personId.value_or(0);
  if (person)
    entry.identity = input.evaluated.known ? "known" : "unknown";
  entry.identityConfidence = input.evaluated.identityConfidence;
  entry.zoneKind = input.evaluated.zoneKind;
  entry.trackId = object.trackId;
  if (track) {
    entry.firstSeenMs = track->firstSeenMs;
    entry.lastSeenMs = track->lastSeenMs;
    entry.dwellMs = input.stamp - track->firstSeenMs;
    entry.signature = track->signature;
    entry.observationId = std::to_string(event.cameraId) + ":" +
                          std::to_string(object.trackId) + ":" +
                          std::to_string(track->firstSeenMs);
  }
  event.objects.push_back(std::move(entry));
}

void CameraOperatorService::mergePersonPending(const PersonPendingInput& input)
{
  CameraState& state = input.state;
  ObjectDetectedEvent& event = state.pendingPersons[input.trackId];
  if (event.cameraId == 0) {
    state.pendingPersonStartMs[input.trackId] = input.stamp;
    event.cameraId = input.cameraId;
    event.cameraName = input.cameraName;
    event.capturedAtMs = input.stamp;
    event.frameWidth = input.frameWidth;
    event.frameHeight = input.frameHeight;
    event.rule = input.outcome.rule;
    event.severity = severityName(input.outcome.severity);
    event.escalated = input.outcome.escalated;
    event.knownPersonId = input.outcome.knownPersonId;
  }
  else if (severityRank(severityName(input.outcome.severity)) >
           severityRank(event.severity)) {
    event.rule = input.outcome.rule;
    event.severity = severityName(input.outcome.severity);
    event.escalated = input.outcome.escalated;
    event.knownPersonId = input.outcome.knownPersonId;
  }
  event.dwellMs = input.dwellMs;
  event.trackId = input.trackId;
  for (const auto& evaluated : input.outcome.objects)
    mergeObject({.event = event,
                 .evaluated = evaluated,
                 .state = state,
                 .stamp = input.stamp});
}

void CameraOperatorService::publishPersonPending(
    const PublishPersonInput& input)
{
  auto pending = input.state.pendingPersons.find(input.trackId);
  if (pending == input.state.pendingPersons.end())
    return;
  if (pending->second.eventId.empty()) {
    pending->second.eventId = std::to_string(input.cameraId) + ":" +
                              std::to_string(input.nowMs) + ":" +
                              std::to_string(input.state.nextEventSeq++);
  }
  ObjectDetectedEvent event = pending->second;
  event.detectedAtMs = input.nowMs;
  event.publishedAtMs = input.nowMs;
  const ObjectEventPublishResult result =
      inputs_.dependencies.sink
          ? inputs_.dependencies.sink->publish(event)
          : ObjectEventPublishResult::Failed;
  if (result == ObjectEventPublishResult::Failed) {
    LOG_ERROR << "Camera operator: outbox enqueue failed for track "
              << input.trackId << "; keeping the event pending";
    return;
  }
  input.state.pendingPersons.erase(pending);
  input.state.pendingPersonStartMs.erase(input.trackId);
  if (result == ObjectEventPublishResult::Recorded) {
    const auto track = input.state.personTracks.find(input.trackId);
    if (track != input.state.personTracks.end())
      track->second.lastEmitMs = input.nowMs;
    EvidenceUploader::instance().uploadDetection(input.cameraId, input.nowMs);
  }
}

void CameraOperatorService::publishOtherPending(const PublishOtherInput& input)
{
  if (!input.state.pendingOther)
    return;
  if (input.state.pendingOther->eventId.empty()) {
    input.state.pendingOther->eventId =
        std::to_string(input.cameraId) + ":" + std::to_string(input.nowMs) +
        ":" + std::to_string(input.state.nextEventSeq++);
  }
  ObjectDetectedEvent event = *input.state.pendingOther;
  event.detectedAtMs = input.nowMs;
  event.publishedAtMs = input.nowMs;
  const ObjectEventPublishResult result =
      inputs_.dependencies.sink
          ? inputs_.dependencies.sink->publish(event)
          : ObjectEventPublishResult::Failed;
  if (result == ObjectEventPublishResult::Failed) {
    LOG_ERROR << "Camera operator: outbox enqueue failed for a class event; "
                 "keeping the event pending";
    return;
  }
  input.state.pendingOther.reset();
  input.state.pendingOtherStartMs = 0;
  if (result == ObjectEventPublishResult::Recorded)
    EvidenceUploader::instance().uploadDetection(input.cameraId, input.nowMs);
}
