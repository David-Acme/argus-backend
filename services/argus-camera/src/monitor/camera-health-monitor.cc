#include <monitor/camera-health-monitor.hxx>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <operator/frame-source.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <thread>

namespace
{
constexpr int kSampleWidth = 160;
constexpr int kSampleHeight = 90;

int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace

CameraHealthState health_monitor::classify(const HealthMetrics& metrics,
                                      const HealthThresholds& thresholds)
{
  if (metrics.brightness < thresholds.dark)
    return CameraHealthState::Dark;
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
  }
  return "ok";
}

CameraHealthMonitor::CameraHealthMonitor(Dependencies dependencies,
                                         CameraHealthConfig config)
    : dependencies_(dependencies), config_(config)
{
}

CameraHealthMonitor::~CameraHealthMonitor()
{
  stop();
}

void CameraHealthMonitor::start()
{
  if (running_.exchange(true))
    return;
  LOG_INFO << "Camera health monitor: every " << config_.intervalMs
           << " ms per camera";
  drogon::async_run([this]() -> drogon::Task<void> {
    co_await run();
  });
}

void CameraHealthMonitor::stop()
{
  running_.store(false);
}

std::vector<CameraHealthMonitor::CameraRef>
CameraHealthMonitor::loadCameras() const
{
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

  if (state.reference.size() != input.rgb.size()) {
    const bool sane = metrics.brightness >= config_.thresholds.dark &&
                      metrics.brightness <= config_.thresholds.bright &&
                      metrics.blur >= config_.thresholds.blur;
    if (sane)
      state.reference = input.rgb;
    metrics.sceneDiff = 0.0;
    return metrics;
  }

  int64_t diff = 0;
  for (size_t i = 0; i < input.rgb.size(); ++i)
    diff += std::abs(static_cast<int>(input.rgb[i]) -
                     static_cast<int>(state.reference[i]));
  metrics.sceneDiff =
      static_cast<double>(diff) / (255.0 * static_cast<double>(input.rgb.size()));
  return metrics;
}

drogon::Task<void> CameraHealthMonitor::run()
{
  while (running_.load()) {
    const auto cameras = co_await BlockingTask<std::vector<CameraRef>>(
        [this]() { return loadCameras(); });
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      for (auto it = states_.begin(); it != states_.end();) {
        const bool present =
            std::any_of(cameras.begin(), cameras.end(),
                        [id = it->first](const CameraRef& camera) {
                          return camera.id == id;
                        });
        if (!present)
          it = states_.erase(it);
        else
          ++it;
      }
    }
    for (const auto& camera : cameras) {
      if (!running_.load())
        break;
      auto frame = co_await dependencies_.source->grab(
          {.cameraId = camera.id, .cameraName = camera.name});

      CameraHealthState status = CameraHealthState::Unreachable;
      HealthMetrics metrics;
      int64_t capturedAt = nowMs();
      if (frame) {
        const cv::Mat raw = cv::imdecode(frame->jpeg, cv::IMREAD_COLOR);
        if (!raw.empty()) {
          cv::Mat small;
          cv::resize(raw, small, cv::Size(kSampleWidth, kSampleHeight), 0, 0,
                     cv::INTER_AREA);
          std::vector<uint8_t> gray(small.total());
          const cv::Mat grayMat(kSampleHeight, kSampleWidth, CV_8UC1,
                                gray.data());
          cv::cvtColor(small, grayMat, cv::COLOR_BGR2GRAY);

          {
            std::lock_guard<std::mutex> lock(stateMutex_);
            CameraState& state = states_[camera.id];
            metrics = measure(
                {.camera = camera,
                 .rgb = std::move(gray),
                 .width = kSampleWidth,
                 .height = kSampleHeight,
                 .capturedAtMs = capturedAt},
                state);
            status = health_monitor::classify(metrics, config_.thresholds);
          }
          capturedAt = frame->capturedAtMs;
        }
      }

      bool transitioned = false;
      {
        std::lock_guard<std::mutex> lock(stateMutex_);
        CameraState& state = states_[camera.id];
        if (state.lastStatus != status) {
          state.lastStatus = status;
          transitioned = true;
        }
      }
      if (!transitioned)
        continue;

      if (status == CameraHealthState::Ok)
        LOG_INFO << "Camera health: " << camera.id << " recovered";
      else
        LOG_WARN << "Camera health: " << camera.id << " is "
                 << health_monitor::statusName(status);
      if (dependencies_.sink) {
        dependencies_.sink->publish({.cameraId = camera.id,
                                     .cameraName = camera.name,
                                     .status = status,
                                     .metrics = metrics,
                                     .detectedAtMs = capturedAt});
      }
    }

    co_await BlockingTask<void>([this]() {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(config_.intervalMs));
    });
  }
  co_return;
}
