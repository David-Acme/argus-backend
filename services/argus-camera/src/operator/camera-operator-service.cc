#include <operator/camera-operator-service.hxx>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
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
} // namespace

CameraOperatorService::CameraOperatorService(Inputs inputs)
    : inputs_(std::move(inputs))
{
}

void CameraOperatorService::start()
{
  if (running_.exchange(true))
    return;

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
             << e.what() << "); no cameras analyzed";
    running_.store(false);
    return;
  }

  LOG_INFO << "Camera operator: started for " << cameras.size()
           << " camera(s) at " << inputs_.objects.maxFpsInference
           << " fps inference budget";
  for (const auto& camera : cameras) {
    drogon::async_run([this, camera]() -> drogon::Task<void> {
      co_await runCamera(camera);
    });
  }
}

void CameraOperatorService::stop()
{
  running_.store(false);
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

drogon::Task<void> CameraOperatorService::runCamera(CameraRef camera)
{
  const int64_t intervalMs =
      static_cast<int64_t>(1000.0 / inputs_.objects.maxFpsInference);
  while (running_.load()) {
    const int64_t tickStart = nowMs();
    auto frame = co_await inputs_.dependencies.source->grab(
        {.cameraId = camera.id, .cameraName = camera.name});
    if (frame) {
      co_await BlockingTask<void>{[this, &camera, &frame]() {
        processFrame(camera.id, camera.name, *frame);
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

void CameraOperatorService::processFrame(int64_t cameraId,
                                         const std::string& cameraName,
                                         CameraFrame& frame)
{
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

  auto objects = inputs_.dependencies.detector->detect(
      reinterpret_cast<const uint8_t*>(rgb.data), rgb.cols, rgb.rows);

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
  intelligence.objects = objects;
  for (const auto& zone : inputs_.operator_.zones) {
    if (zone.cameraId == cameraId)
      intelligence.zones.push_back(zone);
  }
  intelligence.ignoredClasses = inputs_.operator_.ignoredClasses;
  intelligence.night = isNightHour(hour);
  intelligence.state = operatorState;
  intelligence.matcher = inputs_.dependencies.matcher;
  intelligence.frameRgb = rgb.data;
  intelligence.frameWidth = rgb.cols;
  intelligence.frameHeight = rgb.rows;

  const auto outcome = EventIntelligence::evaluate(intelligence);

  if (hasVehicle)
    state.vehicleLastSeenMs = stamp;

  if (outcome.publish) {
    if (!state.pending) {
      state.pending = ObjectDetectedEvent{};
      state.pendingStartMs = stamp;
      state.pending->cameraId = cameraId;
      state.pending->cameraName = cameraName;
      state.pending->frameWidth = rgb.cols;
      state.pending->frameHeight = rgb.rows;
      state.pending->rule = outcome.rule;
      state.pending->severity = severityName(outcome.severity);
      state.pending->escalated = outcome.escalated;
      state.pending->knownPersonId = outcome.knownPersonId;
    } else {
      if (severityRank(severityName(outcome.severity)) >
          severityRank(state.pending->severity)) {
        state.pending->rule = outcome.rule;
        state.pending->severity = severityName(outcome.severity);
        state.pending->escalated = outcome.escalated;
        state.pending->knownPersonId = outcome.knownPersonId;
      }
    }
    for (const auto& object : objects) {
      const bool alreadyPending =
          std::any_of(state.pending->objects.begin(),
                      state.pending->objects.end(),
                      [&](const DetectedEventObject& entry) {
                        return entry.name == object.name;
                      });
      if (alreadyPending)
        continue;
      DetectedEventObject entry;
      entry.name = object.name;
      entry.confidence = object.confidence;
      entry.x = object.x;
      entry.y = object.y;
      entry.w = object.w;
      entry.h = object.h;
      state.pending->objects.push_back(std::move(entry));
    }
  }

  if (state.pending &&
      stamp - state.pendingStartMs >= inputs_.operator_.aggregationWindowMs)
    publishPending(cameraId, state, stamp);
}

void CameraOperatorService::publishPending(int64_t cameraId,
                                           CameraState& state, int64_t nowMs)
{
  auto event = *state.pending;
  state.pending.reset();

  for (const auto& object : event.objects) {
    const auto last = state.lastEmitByClass.find(object.name);
    if (last != state.lastEmitByClass.end() &&
        nowMs - last->second < inputs_.operator_.cooldownMs) {
      LOG_INFO << "Camera operator: event suppressed by cooldown (camera "
               << cameraId << ", class " << object.name << ")";
      return;
    }
  }

  event.detectedAtMs = nowMs;
  for (const auto& object : event.objects)
    state.lastEmitByClass[object.name] = nowMs;
  if (inputs_.dependencies.sink)
    inputs_.dependencies.sink->publish(event);
}
