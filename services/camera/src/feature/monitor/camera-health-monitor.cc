#include <feature/monitor/camera-health-monitor.hxx>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <shared/services/stream/camera-live-board.hxx>
#include <shared/services/stream/frame-source.hxx>
#include <shared/services/camera-driver/camera-scene-log.hxx>
#include <sqlite/db-service.hxx>
#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <span>
#include <utility>

namespace
{
constexpr int kSampleWidth = 160;
constexpr int64_t kRescanMs = 5000;
constexpr int kSampleHeight = 90;
constexpr int64_t kFrameMaxAgeMs = 1000;

constexpr double kInvertedSpread = 1.5957691216057308;
constexpr double kFlatDeviation = 1.0;
constexpr int kDriftWeight = 8;

struct Spread
{
  double mean{0.0};
  double deviation{0.0};
};

Spread spreadOf(std::span<const uint8_t> pixels)
{
  double sum = 0.0;
  double squares = 0.0;
  for (const uint8_t pixel : pixels) {
    sum += pixel;
    squares += static_cast<double>(pixel) * pixel;
  }
  const auto count = static_cast<double>(pixels.size());
  const double mean = sum / count;
  return {.mean = mean, .deviation = std::sqrt(std::max(0.0, squares / count - mean * mean))};
}

double sceneDistance(std::span<const uint8_t> current, std::span<const uint8_t> reference)
{
  if (current.empty() || current.size() != reference.size())
    return 0.0;
  const Spread a = spreadOf(current);
  const Spread b = spreadOf(reference);
  double total = 0.0;
  if (a.deviation < kFlatDeviation || b.deviation < kFlatDeviation) {
    for (size_t i = 0; i < current.size(); ++i)
      total += std::abs(static_cast<double>(current[i]) - reference[i]) / 255.0;
    return total / static_cast<double>(current.size());
  }
  for (size_t i = 0; i < current.size(); ++i)
    total += std::abs((current[i] - a.mean) / a.deviation -
                      (reference[i] - b.mean) / b.deviation);
  return std::min(1.0, total / static_cast<double>(current.size()) / kInvertedSpread);
}

void drift(std::vector<uint8_t>& reference, std::span<const uint8_t> current)
{
  for (size_t i = 0; i < reference.size(); ++i)
    reference[i] = static_cast<uint8_t>(
        (reference[i] * (kDriftWeight - 1) + current[i] + kDriftWeight / 2) / kDriftWeight);
}

int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

CameraHealthState health_monitor::classify(const HealthMetrics& metrics,
                                      const HealthThresholds& thresholds)
{
  if (metrics.brightness < thresholds.dark)
    return metrics.blur < thresholds.blur ? CameraHealthState::Covered
                                          : CameraHealthState::Dark;
  if (metrics.brightness > thresholds.bright)
    return CameraHealthState::Bright;
  if (metrics.blur < thresholds.blur)
    return CameraHealthState::Blurred;
  if (metrics.sceneDiff > thresholds.sceneDiff)
    return CameraHealthState::Moved;
  return CameraHealthState::Ok;
}

std::string health_monitor::statusName(CameraHealthState status)
{
  switch (status) {
    case CameraHealthState::Ok:
      return "ok";
    case CameraHealthState::Dark:
      return "dark";
    case CameraHealthState::Bright:
      return "bright";
    case CameraHealthState::Blurred:
      return "blurred";
    case CameraHealthState::Moved:
      return "moved";
    case CameraHealthState::Unreachable:
      return "unreachable";
    case CameraHealthState::Covered:
      return "covered";
  }
  return "ok";
}

CameraHealthMonitor::CameraHealthMonitor(Dependencies dependencies,
                                         CameraHealthConfig config)
    : dependencies_(std::move(dependencies)), config_(std::move(config))
{
}

CameraHealthMonitor::~CameraHealthMonitor()
{
  requestStop();
}

void CameraHealthMonitor::start()
{
  if (running_.exchange(true))
    return;
  LOG_INFO << "Camera health monitor: every " << config_.intervalMs
           << " ms per camera";
  drogon::async_run([this]() -> drogon::Task<void> {
    try {
      co_await run();
    }
    catch (const std::exception& error) {
      LOG_WARN << "Camera health monitor: run loop failed: " << error.what();
    }
    catch (...) {
      LOG_WARN << "Camera health monitor: run loop failed with unknown error";
    }
    co_return;
  });
}

void CameraHealthMonitor::requestStop()
{
  running_.store(false);
}

bool CameraHealthMonitor::drained() const
{
  return inFlight_.load(std::memory_order_acquire) == 0;
}

std::vector<CameraHealthMonitor::CameraRef>
CameraHealthMonitor::loadCameras()
{
  const in_flight::Guard guard(inFlight_);
  std::vector<CameraRef> cameras;
  try {
    const auto rows = DbService::client()->execSqlSync(
        "SELECT id, name FROM camera WHERE deleted_at IS NULL AND is_enabled = 1");
    for (const auto& row : rows)
      cameras.push_back({row["id"].as<int64_t>(), row["name"].as<std::string>()});
  }
  catch (const std::exception& e) {
    LOG_WARN << "Camera health: camera discovery failed (" << e.what() << ")";
  }
  return cameras;
}

HealthMetrics CameraHealthMonitor::measure(const TickInput& input,
                                           CameraState& state) const
{
  HealthMetrics metrics;
  const cv::Mat gray(input.height, input.width, CV_8UC1,
                     const_cast<uint8_t*>(input.rgb.data()));

  cv::Scalar mean;
  cv::Scalar stddev;
  cv::meanStdDev(gray, mean, stddev);
  metrics.brightness = mean[0];

  cv::Mat laplacian;
  cv::Laplacian(gray, laplacian, CV_64F);
  cv::Scalar lapMean;
  cv::Scalar lapStd;
  cv::meanStdDev(laplacian, lapMean, lapStd);
  metrics.blur = lapStd[0] * lapStd[0];

  const std::span<const uint8_t> current(input.rgb);
  const bool sane = metrics.brightness >= config_.thresholds.dark &&
                    metrics.brightness <= config_.thresholds.bright &&
                    metrics.blur >= config_.thresholds.blur;
  const auto adopt = [&state, &input] {
    state.reference = input.rgb;
    state.referenceAtMs = input.capturedAtMs;
    state.newSceneSinceMs = 0;
  };

  const CameraScene scene = CameraSceneLog::instance().sceneOf(input.camera.id);
  const bool settling = scene.aimedAtMs > 0 &&
                        input.capturedAtMs < scene.aimedAtMs + 2 * config_.intervalMs;
  if (settling || scene.aimedAtMs > state.referenceAtMs)
    state.reference.clear();

  if (state.reference.size() != input.rgb.size()) {
    if (sane)
      adopt();
    state.previous = input.rgb;
    metrics.sceneDiff = 0.0;
    return metrics;
  }

  metrics.sceneDiff = sceneDistance(current, state.reference);
  const double threshold = config_.thresholds.sceneDiff;
  if (metrics.sceneDiff <= threshold) {
    state.newSceneSinceMs = 0;
    if (sane && metrics.sceneDiff <= threshold / 2.0)
      drift(state.reference, current);
  }
  else if (!sane || sceneDistance(current, state.previous) > threshold / 2.0) {
    state.newSceneSinceMs = input.capturedAtMs;
  }
  else if (state.newSceneSinceMs == 0) {
    state.newSceneSinceMs = input.capturedAtMs;
  }
  else if (input.capturedAtMs - state.newSceneSinceMs >= config_.rebaselineAfterMs) {
    LOG_INFO << "Camera health: " << input.camera.id
             << " has kept its new view; it is the reference now";
    adopt();
    metrics.sceneDiff = 0.0;
  }
  state.previous = input.rgb;
  return metrics;
}

bool CameraHealthMonitor::dueForSample(int64_t cameraId)
{
  const int64_t now = nowMs();
  std::scoped_lock lock(stateMutex_);
  const auto it = states_.find(cameraId);
  return it == states_.end() || !it->second.sampled ||
         now - it->second.sampledAtMs >= config_.intervalMs;
}

drogon::Task<void> CameraHealthMonitor::run()
{
  const int64_t rescanMs = std::min<int64_t>(kRescanMs, config_.intervalMs);
  while (running_.load()) {
    if (dependencies_.active && !dependencies_.active()) {
      if (!idle_.exchange(true)) {
        std::scoped_lock lock(stateMutex_);
        states_.clear();
        LOG_INFO << "Camera health monitor: surveillance is disabled; idle until it is enabled again";
      }
      co_await drogon::sleepCoro(drogon::app().getLoop(),
                                 std::chrono::milliseconds(rescanMs));
      continue;
    }
    idle_.store(false);
    const auto cameras = co_await BlockingTask<std::vector<CameraRef>>(
        [this]() { return loadCameras(); });
    {
      std::scoped_lock lock(stateMutex_);
      std::erase_if(states_, [&cameras](const auto& entry) {
        return std::ranges::none_of(cameras, [id = entry.first](const CameraRef& camera) {
          return camera.id == id;
        });
      });
    }
    for (const auto& camera : cameras) {
      if (!running_.load())
        break;
      if (!dueForSample(camera.id))
        continue;
      try {
        co_await tick(camera);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Camera health: sampling camera " << camera.id
                 << " failed (" << error.what() << ")";
      }
    }
    if (running_.load())
      co_await drogon::sleepCoro(drogon::app().getLoop(),
                                 std::chrono::milliseconds(rescanMs));
  }
  co_return;
}

drogon::Task<void> CameraHealthMonitor::tick(CameraRef camera)
{
  if (CameraSceneLog::instance().sceneOf(camera.id).privacy)
    co_return;
  auto frame = co_await dependencies_.source->grab(
      {.cameraId = camera.id, .cameraName = camera.name, .maxAgeMs = kFrameMaxAgeMs});

  struct Sample
  {
    CameraHealthState status{CameraHealthState::Unreachable};
    HealthMetrics metrics;
    int64_t capturedAt{0};
    bool reachable{false};
    int frameWidth{0};
    int frameHeight{0};
  };
  Sample sample{.status = CameraHealthState::Unreachable,
                .metrics = {},
                .capturedAt = nowMs(),
                .reachable = false,
                .frameWidth = 0,
                .frameHeight = 0};
  if (frame && !frame->jpeg.empty()) {
    auto measureFrame =
        [this, &camera, jpeg = std::move(frame->jpeg), capturedAt = frame->capturedAtMs]() {
          Sample measured{.status = CameraHealthState::Unreachable,
                          .metrics = {},
                          .capturedAt = capturedAt,
                          .reachable = false,
                          .frameWidth = 0,
                          .frameHeight = 0};
          const cv::Mat raw = cv::imdecode(jpeg, cv::IMREAD_COLOR);
          if (raw.empty())
            return measured;
          measured.reachable = true;
          measured.frameWidth = raw.cols;
          measured.frameHeight = raw.rows;
          cv::Mat small;
          cv::resize(raw, small, cv::Size(kSampleWidth, kSampleHeight), 0, 0, cv::INTER_AREA);
          std::vector<uint8_t> gray(small.total());
          const cv::Mat grayMat(kSampleHeight, kSampleWidth, CV_8UC1, gray.data());
          cv::cvtColor(small, grayMat, cv::COLOR_BGR2GRAY);

          std::scoped_lock lock(stateMutex_);
          CameraState& state = states_[camera.id];
          measured.metrics = measure({.camera = camera,
                                      .rgb = std::move(gray),
                                      .width = kSampleWidth,
                                      .height = kSampleHeight,
                                      .capturedAtMs = capturedAt},
                                     state);
          measured.status = health_monitor::classify(measured.metrics, config_.thresholds);
          return measured;
        };
    if (drogon::app().isRunning())
      sample = co_await BlockingTask<Sample>(std::move(measureFrame), BlockingLane::Heavy);
    else
      sample = measureFrame();
  }
  CameraHealthState status = sample.status;
  const HealthMetrics& metrics = sample.metrics;
  const int64_t capturedAt = sample.capturedAt;
  const bool reachable = sample.reachable;
  const int frameWidth = sample.frameWidth;
  const int frameHeight = sample.frameHeight;

  bool transitioned = false;
  bool heartbeatDue = false;
  {
    std::scoped_lock lock(stateMutex_);
    CameraState& state = states_[camera.id];
    state.sampledAtMs = capturedAt;
    state.sampled = true;
    state.misses = reachable ? 0 : state.misses + 1;
    const bool held = !reachable && state.misses < kMissesBeforeOffline && state.published;
    if (held) {
      status = state.lastStatus;
    }
    else if (state.lastStatus != status) {
      state.lastStatus = status;
      transitioned = true;
    }
    else if (!state.published ||
             capturedAt - state.lastPublishMs >= config_.intervalMs) {
      heartbeatDue = true;
    }
    if (transitioned || heartbeatDue) {
      state.lastPublishMs = capturedAt;
      state.published = true;
    }
  }

  CameraLiveBoard::instance().recordSample({.cameraId = camera.id,
                                            .reachable = reachable,
                                            .health = health_monitor::statusName(status),
                                            .atMs = capturedAt,
                                            .width = frameWidth,
                                            .height = frameHeight});

  if (dependencies_.presence) {
    const in_flight::Guard guard(inFlight_);
    co_await dependencies_.presence->record(
        {.cameraId = camera.id, .reachable = reachable});
  }

  if (!transitioned && !heartbeatDue)
    co_return;

  if (transitioned) {
    if (status == CameraHealthState::Ok)
      LOG_INFO << "Camera health: " << camera.id << " is ok";
    else
      LOG_WARN << "Camera health: " << camera.id << " is "
               << health_monitor::statusName(status);
  }
  if (dependencies_.sink) {
    dependencies_.sink->publish({.cameraId = camera.id,
                                 .cameraName = camera.name,
                                 .status = status,
                                 .metrics = metrics,
                                 .detectedAtMs = capturedAt});
  }
  co_return;
}
