#include <feature/monitor/camera-health-monitor.hxx>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <shared/services/stream/camera-live-board.hxx>
#include <shared/services/stream/frame-source.hxx>
#include <shared/services/camera-driver/camera-driver.hxx>
#include <shared/services/camera-driver/camera-scene-log.hxx>
#include <text/json-util.hxx>
#include <json/value.h>
#include <shared/services/privacy-state/privacy-state.hxx>
#include <runtime/blocking-task.hxx>
#include <sqlite/db-service.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
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
constexpr int64_t kPrivacyBackoffMs = 600000;
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

constexpr int kSaturatedLevel = 245;
constexpr double kTexturedBlock = 8.0;
constexpr double kFlatBlock = 2.0;
constexpr double kLostBlockShare = 0.25;
constexpr double kWholeFrameLost = 0.9;
constexpr double kSuddenMargin = 15.0;
constexpr double kSteadySharpness = 0.8;
constexpr double kShiftResponse = 0.2;
constexpr double kMinLaplacian = 1.0;

struct Texture
{
  double mean{0.0};
  double deviation{0.0};
  double laplacian{0.0};
  double entropy{0.0};
  double saturated{0.0};
  std::array<double, 16> blocks{};
};

double entropyOf(const cv::Mat& gray)
{
  std::array<double, 256> histogram{};
  for (int row = 0; row < gray.rows; ++row) {
    const uint8_t* line = gray.ptr<uint8_t>(row);
    for (int column = 0; column < gray.cols; ++column)
      histogram.at(line[column]) += 1.0;
  }
  const auto total = static_cast<double>(gray.total());
  double entropy = 0.0;
  for (const double count : histogram) {
    if (count <= 0.0)
      continue;
    const double share = count / total;
    entropy -= share * std::log2(share);
  }
  return entropy;
}

Texture textureOf(const cv::Mat& gray, int side)
{
  Texture texture;
  cv::Scalar mean;
  cv::Scalar deviation;
  cv::meanStdDev(gray, mean, deviation);
  texture.mean = mean[0];
  texture.deviation = deviation[0];
  cv::Mat laplacian;
  cv::Laplacian(gray, laplacian, CV_64F);
  cv::Scalar lapMean;
  cv::Scalar lapDeviation;
  cv::meanStdDev(laplacian, lapMean, lapDeviation);
  texture.laplacian = lapDeviation[0] * lapDeviation[0];
  texture.entropy = entropyOf(gray);
  texture.saturated = static_cast<double>(cv::countNonZero(gray >= kSaturatedLevel)) /
                      static_cast<double>(gray.total());
  const int blockWidth = gray.cols / side;
  const int blockHeight = gray.rows / side;
  for (int row = 0; row < side; ++row) {
    for (int column = 0; column < side; ++column) {
      const int width = column == side - 1 ? gray.cols - column * blockWidth : blockWidth;
      const int height = row == side - 1 ? gray.rows - row * blockHeight : blockHeight;
      cv::Scalar blockMean;
      cv::Scalar blockDeviation;
      cv::meanStdDev(gray(cv::Rect(column * blockWidth, row * blockHeight, width, height)), blockMean,
                     blockDeviation);
      const int block = row * side + column;
      texture.blocks.at(static_cast<size_t>(block)) = blockDeviation[0];
    }
  }
  return texture;
}

double sharpnessOf(const Texture& texture)
{
  return texture.deviation >= kFlatDeviation ? std::sqrt(texture.laplacian) / texture.deviation : 0.0;
}

struct BlockPair
{
  const std::array<double, 16>& current;
  const std::array<double, 16>& reference;
};

double blocksLost(const BlockPair& blocks)
{
  const std::array<double, 16>& current = blocks.current;
  const std::array<double, 16>& reference = blocks.reference;
  int textured = 0;
  int lost = 0;
  for (size_t index = 0; index < reference.size(); ++index) {
    if (reference.at(index) < kTexturedBlock)
      continue;
    ++textured;
    if (current.at(index) < std::max(kFlatBlock, reference.at(index) * kLostBlockShare))
      ++lost;
  }
  return textured == 0 ? 0.0 : static_cast<double>(lost) / textured;
}

struct ShiftInput
{
  std::span<const uint8_t> reference;
  std::span<const uint8_t> current;
  int width{0};
  int height{0};
};

double shiftOf(const ShiftInput& input)
{
  if (input.reference.size() != input.current.size() || input.reference.empty())
    return 0.0;
  cv::Mat reference;
  cv::Mat current;
  cv::Mat(input.height, input.width, CV_8UC1, const_cast<uint8_t*>(input.reference.data()))
      .convertTo(reference, CV_32F);
  cv::Mat(input.height, input.width, CV_8UC1, const_cast<uint8_t*>(input.current.data()))
      .convertTo(current, CV_32F);
  cv::Mat window;
  cv::createHanningWindow(window, reference.size(), CV_32F);
  double response = 0.0;
  const cv::Point2d shift = cv::phaseCorrelate(reference, current, window, &response);
  if (response < kShiftResponse)
    return 0.0;
  return std::hypot(shift.x, shift.y);
}

double chromaOf(const cv::Mat& bgr)
{
  std::vector<cv::Mat> planes;
  cv::split(bgr, planes);
  if (planes.size() < 3)
    return 0.0;
  cv::Mat blueGreen;
  cv::Mat greenRed;
  cv::absdiff(planes.at(0), planes.at(1), blueGreen);
  cv::absdiff(planes.at(1), planes.at(2), greenRed);
  return (cv::mean(blueGreen)[0] + cv::mean(greenRed)[0]) / 2.0;
}

std::string_view linkOf(const std::string& config)
{
  const Json::Value parsed = json_util::fromString(config);
  if (parsed.isObject() && parsed.get("link", "").asString() == "wired")
    return "wired";
  return "wifi";
}
}

CameraHealthState health_monitor::classify(const HealthMetrics& metrics,
                                      const HealthThresholds& thresholds)
{
  if (metrics.brightness < thresholds.dark) {
    if (metrics.blur < thresholds.blur || metrics.entropy < thresholds.coverEntropy)
      return CameraHealthState::Covered;
    return metrics.sudden ? CameraHealthState::DarkSudden : CameraHealthState::Dark;
  }
  if (metrics.saturated >= thresholds.saturatedShare) {
    if (metrics.edgeRatio < thresholds.whiteoutEdgeRatio)
      return CameraHealthState::Whiteout;
    if (metrics.edgeRatio < thresholds.blindEdgeRatio)
      return CameraHealthState::Blinded;
  }
  if (metrics.brightness > thresholds.bright)
    return CameraHealthState::Bright;
  if (metrics.entropy < thresholds.coverEntropy && metrics.edgeRatio < thresholds.coverEdgeRatio)
    return CameraHealthState::Covered;
  if (metrics.blocksLost >= kWholeFrameLost)
    return CameraHealthState::Covered;
  if (metrics.blocksLost >= thresholds.partialBlockShare)
    return CameraHealthState::PartialCover;
  if (metrics.blur < thresholds.blur || metrics.sharpness < thresholds.defocusSharpness)
    return CameraHealthState::Blurred;
  if (metrics.sceneDiff > thresholds.sceneDiff || metrics.shiftPx > thresholds.reaimShiftPx)
    return CameraHealthState::Moved;
  return CameraHealthState::Ok;
}

std::string_view health_monitor::statusName(CameraHealthState status)
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
    case CameraHealthState::Whiteout:
      return "whiteout";
    case CameraHealthState::Blinded:
      return "blinded";
    case CameraHealthState::PartialCover:
      return "partial_cover";
    case CameraHealthState::DarkSudden:
      return "dark_sudden";
  }
  return "ok";
}

bool health_monitor::tampering(CameraHealthState status, bool sudden)
{
  switch (status) {
    case CameraHealthState::Covered:
    case CameraHealthState::Whiteout:
    case CameraHealthState::Blinded:
    case CameraHealthState::PartialCover:
    case CameraHealthState::Moved:
    case CameraHealthState::DarkSudden:
    case CameraHealthState::Unreachable:
      return true;
    case CameraHealthState::Blurred:
      return sudden;
    case CameraHealthState::Ok:
    case CameraHealthState::Dark:
    case CameraHealthState::Bright:
      return false;
  }
  return false;
}

CameraHealthMonitor::CameraHealthMonitor(Dependencies dependencies,
                                         CameraHealthConfig config)
    : dependencies_(std::move(dependencies)), config_(config)
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
        "SELECT * FROM camera WHERE deleted_at IS NULL AND is_enabled = 1");
    for (const auto& row : rows)
    {
      CameraSchema camera(row);
      cameras.push_back(
          {.id = camera.id, .name = camera.name, .camera = std::move(camera)});
    }
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
  const HealthThresholds& thresholds = config_.thresholds;
  const cv::Mat gray(input.height, input.width, CV_8UC1,
                     const_cast<uint8_t*>(input.rgb.data()));
  const Texture texture = textureOf(gray, kGridSide);
  metrics.brightness = texture.mean;
  metrics.blur = texture.laplacian;
  metrics.entropy = texture.entropy;
  metrics.saturated = texture.saturated;
  metrics.chroma = input.chroma;
  metrics.infrared = input.chroma < thresholds.infraredChroma;
  const double sharpness = sharpnessOf(texture);

  const auto remember = [&state, &metrics](double sharpnessRatio) {
    state.previousBrightness = metrics.brightness;
    state.previousSharpness = sharpnessRatio;
    state.previousKnown = true;
  };

  if (state.infraredKnown && state.infrared != metrics.infrared)
    state.holdUntilMs = input.capturedAtMs + config_.dayNightHoldMs;
  state.infrared = metrics.infrared;
  state.infraredKnown = true;
  Reference& reference = state.references.at(metrics.infrared ? 1 : 0);

  const std::span<const uint8_t> current(input.rgb);
  const bool sane = metrics.brightness >= thresholds.dark &&
                    metrics.brightness <= thresholds.bright &&
                    metrics.blur >= thresholds.blur &&
                    metrics.saturated < thresholds.saturatedShare;
  const auto adopt = [&state, &input, &reference, &texture, sharpness] {
    reference.gray = input.rgb;
    reference.atMs = input.capturedAtMs;
    reference.blocks = texture.blocks;
    reference.laplacian = texture.laplacian;
    reference.sharpness = sharpness;
    state.newSceneSinceMs = 0;
  };
  const auto pause = [&state, &input, &metrics, &remember] {
    state.previous = input.rgb;
    state.suddenSinceMs = 0;
    metrics.sceneDiff = 0.0;
    metrics.paused = true;
    remember(1.0);
  };

  const int64_t intervalMs = input.intervalMs > 0 ? input.intervalMs : config_.intervalMs;
  const CameraScene scene = CameraSceneLog::instance().sceneOf(input.camera.id);
  const bool settlingHome = scene.returnsHome && scene.moveEndedAtMs > 0 &&
                            input.capturedAtMs < scene.moveEndedAtMs + 2 * intervalMs;
  if (scene.moving(input.capturedAtMs) || settlingHome || scene.argusAway) {
    pause();
    return metrics;
  }
  if (input.capturedAtMs < state.holdUntilMs) {
    pause();
    metrics.held = true;
    return metrics;
  }
  const bool settling = scene.aimedAtMs > 0 &&
                        input.capturedAtMs < scene.aimedAtMs + 2 * intervalMs;
  if (settling || scene.aimedAtMs > reference.atMs)
    for (Reference& slot : state.references)
      if (settling || scene.aimedAtMs > slot.atMs)
        slot.gray.clear();

  if (reference.gray.size() != input.rgb.size()) {
    if (sane)
      adopt();
    state.previous = input.rgb;
    metrics.sceneDiff = 0.0;
    state.suddenSinceMs = 0;
    remember(1.0);
    return metrics;
  }

  metrics.sceneDiff = sceneDistance(current, reference.gray);
  metrics.edgeRatio = reference.laplacian >= kMinLaplacian ? texture.laplacian / reference.laplacian : 1.0;
  metrics.sharpness = reference.sharpness > 0.0 ? sharpness / reference.sharpness : 1.0;
  metrics.blocksLost = blocksLost({.current = texture.blocks, .reference = reference.blocks});
  metrics.shiftPx = shiftOf({.reference = reference.gray, .current = current, .width = input.width,
                             .height = input.height});

  const bool droppedDark = metrics.brightness < thresholds.dark && state.previousKnown &&
                           state.previousBrightness >= thresholds.dark + kSuddenMargin;
  const bool droppedSharp = metrics.sharpness < thresholds.defocusSharpness && state.previousKnown &&
                            state.previousSharpness >= kSteadySharpness;
  if (state.suddenSinceMs == 0 && (droppedDark || droppedSharp))
    state.suddenSinceMs = input.capturedAtMs;
  if (metrics.brightness >= thresholds.dark && metrics.sharpness >= thresholds.defocusSharpness)
    state.suddenSinceMs = 0;
  metrics.sudden = state.suddenSinceMs > 0;
  remember(metrics.sharpness);

  const double threshold = thresholds.sceneDiff;
  if (scene.returnsHome && scene.moveEndedAtMs > 0 && state.homeCheckedForMs != scene.moveEndedAtMs) {
    state.homeCheckedForMs = scene.moveEndedAtMs;
    metrics.homeDrift = metrics.sceneDiff > threshold;
  }
  if (metrics.sceneDiff <= threshold) {
    state.newSceneSinceMs = 0;
    if (sane && metrics.sceneDiff <= threshold / 2.0)
      drift(reference.gray, current);
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
    metrics.edgeRatio = 1.0;
    metrics.sharpness = 1.0;
    metrics.blocksLost = 0.0;
    metrics.shiftPx = 0.0;
  }
  state.previous = input.rgb;
  return metrics;
}

int64_t CameraHealthMonitor::intervalNow() const
{
  if (dependencies_.protects && dependencies_.protects())
    return std::min(config_.intervalMs, config_.protectIntervalMs);
  return config_.intervalMs;
}

bool CameraHealthMonitor::dueForSample(int64_t cameraId)
{
  const int64_t now = nowMs();
  std::scoped_lock lock(stateMutex_);
  const auto it = states_.find(cameraId);
  return it == states_.end() || !it->second.sampled ||
         now - it->second.sampledAtMs >= intervalNow();
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

void CameraHealthMonitor::publishPrivacyState(const CameraRef& camera)
{
  if (dependencies_.sink == nullptr ||
      !PrivacyStateService::privacyCapable(camera.camera))
    return;
  dependencies_.sink->publishPrivacy(
      {.cameraId = camera.id,
       .cameraName = camera.name,
       .state = PrivacyStateService::instance().stateOf(camera.id),
       .obeyed = PrivacyStateService::instance().masked(camera.id),
       .detectedAtMs = nowMs()});
}

drogon::Task<bool> CameraHealthMonitor::refreshPrivacy(const CameraRef& camera)
{
  if (!PrivacyStateService::privacyCapable(camera.camera))
    co_return true;
  int64_t backoffUntil = 0;
  {
    std::scoped_lock lock(stateMutex_);
    backoffUntil = states_[camera.id].privacyBackoffUntilMs;
  }
  if (nowMs() < backoffUntil)
    co_return true;
  DriverResult result;
  co_await BlockingTask<void>([&camera, &result]() {
    const auto driver =
        CameraDriverRegistry::instance().driverFor(camera.camera);
    result = driver ? driver->status() : DriverResult{};
  });
  if (!result.ok)
  {
    std::scoped_lock lock(stateMutex_);
    states_[camera.id].privacyBackoffUntilMs = nowMs() + kPrivacyBackoffMs;
    co_return true;
  }
  {
    std::scoped_lock lock(stateMutex_);
    states_[camera.id].privacyBackoffUntilMs = 0;
  }
  co_await PrivacyStateService::instance().refresh(
      {.camera = camera.camera, .driverStatus = result.data, .readable = true});
  co_return true;
}

drogon::Task<void> CameraHealthMonitor::tick(CameraRef camera)
{
  co_await refreshPrivacy(camera);
  publishPrivacyState(camera);
  if (PrivacyStateService::instance().masked(camera.id))
    co_return;
  const int64_t intervalMs = intervalNow();
  const int64_t maxAgeMs =
      intervalMs < config_.intervalMs ? std::max(kFrameMaxAgeMs, intervalMs) : kFrameMaxAgeMs;
  auto frame = co_await dependencies_.source->grab(
      {.cameraId = camera.id,
       .cameraName = camera.name,
       .maxAgeMs = maxAgeMs,
       .stream = camera_stream_role::streamFor(CameraStreamRole::Analysis)});

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
        [this, &camera, intervalMs, jpeg = std::move(frame->jpeg), capturedAt = frame->capturedAtMs]() {
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
          const double chroma = chromaOf(small);

          std::scoped_lock lock(stateMutex_);
          CameraState& state = states_[camera.id];
          measured.metrics = measure({.camera = camera,
                                      .rgb = std::move(gray),
                                      .width = kSampleWidth,
                                      .height = kSampleHeight,
                                      .capturedAtMs = capturedAt,
                                      .chroma = chroma,
                                      .intervalMs = intervalMs},
                                     state);
          measured.status = health_monitor::classify(measured.metrics, config_.thresholds);
          if (measured.status == CameraHealthState::Ok && !measured.metrics.paused) {
            state.lastGoodJpeg = jpeg;
            state.lastGoodAtMs = capturedAt;
          }
          return measured;
        };
    if (drogon::app().isRunning())
      sample = co_await BlockingTask<Sample>(std::move(measureFrame), BlockingLane::Heavy);
    else
      sample = measureFrame();
  }
  CameraHealthState status = sample.status;
  const HealthMetrics& metrics = sample.metrics;
  if (metrics.homeDrift && dependencies_.homeDrift) {
    LOG_WARN << "Camera health: " << camera.id << " came back home to a different view";
    dependencies_.homeDrift(camera.id);
  }
  const int64_t capturedAt = sample.capturedAt;
  const bool reachable = sample.reachable;
  const int frameWidth = sample.frameWidth;
  const int frameHeight = sample.frameHeight;

  bool transitioned = false;
  bool heartbeatDue = false;
  bool sudden = metrics.sudden;
  int64_t onsetAtMs = 0;
  std::optional<TamperOnset> onset;
  {
    std::scoped_lock lock(stateMutex_);
    CameraState& state = states_[camera.id];
    state.sampledAtMs = capturedAt;
    state.sampled = true;
    state.misses = reachable ? 0 : state.misses + 1;
    const bool held = (!reachable && state.misses < kMissesBeforeOffline && state.published) ||
                      (reachable && metrics.paused && state.published);
    const bool wasTampering = health_monitor::tampering(state.lastStatus, state.lastSudden);
    if (held) {
      status = state.lastStatus;
      sudden = state.lastSudden;
    }
    else if (state.lastStatus != status || state.lastSudden != sudden) {
      state.lastStatus = status;
      state.lastSudden = sudden;
      transitioned = true;
    }
    else if (!state.published ||
             capturedAt - state.lastPublishMs >= config_.intervalMs) {
      heartbeatDue = true;
    }
    const bool tampering = health_monitor::tampering(status, sudden);
    if (transitioned && tampering && !wasTampering) {
      state.onsetAtMs = capturedAt;
      onset = TamperOnset{.cameraId = camera.id,
                          .cameraName = camera.name,
                          .status = status,
                          .atMs = capturedAt,
                          .lastGoodJpeg = state.lastGoodJpeg,
                          .lastGoodAtMs = state.lastGoodAtMs};
    }
    if (!tampering)
      state.onsetAtMs = 0;
    onsetAtMs = state.onsetAtMs;
    if (transitioned || heartbeatDue) {
      state.lastPublishMs = capturedAt;
      state.published = true;
    }
  }
  if (onset && dependencies_.tamperOnset) {
    LOG_WARN << "Camera health: " << camera.id << " lost its sight ("
             << health_monitor::statusName(onset->status) << "); the tamper evidence is kept";
    dependencies_.tamperOnset(std::move(*onset));
  }

  CameraLiveBoard::instance().recordSample({.cameraId = camera.id,
                                            .reachable = reachable,
                                            .health = std::string(health_monitor::statusName(status)),
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
    HealthMetrics published = metrics;
    published.sudden = sudden;
    dependencies_.sink->publish({.cameraId = camera.id,
                                 .cameraName = camera.name,
                                 .status = status,
                                 .metrics = published,
                                 .detectedAtMs = capturedAt,
                                 .link = std::string(linkOf(camera.camera.config)),
                                 .onsetAtMs = onsetAtMs});
  }
  co_return;
}
