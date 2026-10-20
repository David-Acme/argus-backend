#include "second-look.hxx"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

namespace
{
constexpr int kMinCropPx = 64;
constexpr int kJpegQuality = 88;
constexpr int64_t kCellsPerCamera = 64;

std::atomic<const SecondLook*>& slot()
{
  static std::atomic<const SecondLook*> installed{nullptr};
  return installed;
}

int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

namespace second_look
{
bool doubtful(const DoubtInput& input)
{
  return input.confidence < input.confirmConfidence || input.boxShare < input.smallBoxShare ||
         (input.unknownPerson && input.protect);
}
}

LookCooldown& LookCooldown::instance()
{
  static LookCooldown cooldown;
  return cooldown;
}

bool LookCooldown::cooling(const LookCooldownKey& key)
{
  const std::scoped_lock lock(mutex_);
  const auto found = until_.find(key.cameraId * kCellsPerCamera + key.cell);
  return found != until_.end() && found->second > key.atMs;
}

void LookCooldown::cool(const LookCooldownKey& key, int64_t forMs)
{
  const std::scoped_lock lock(mutex_);
  until_[key.cameraId * kCellsPerCamera + key.cell] = key.atMs + forMs;
}

SecondLook::SecondLook(SecondLookDependencies dependencies) : dependencies_(dependencies) {}

void SecondLook::install(const SecondLook* look)
{
  slot().store(look, std::memory_order_release);
}

const SecondLook* SecondLook::installed()
{
  return slot().load(std::memory_order_acquire);
}

SecondLook::CropVerdict SecondLook::judge(const CropInput& input) const
{
  CropVerdict verdict;
  cv::Mat bgr = cv::imdecode(input.jpeg, cv::IMREAD_COLOR);
  if (bgr.empty())
    return verdict;
  verdict.decoded = true;
  const SecondLookInput& look = input.look;
  if (look.awayFromHome)
    for (const BlackoutColumns& columns : aim_math::blackout(
             {.position = look.position, .fov = look.fov, .sectors = look.sectors, .width = bgr.cols}))
      cv::rectangle(bgr, cv::Rect(columns.from, 0, columns.to - columns.from, bgr.rows), cv::Scalar(0, 0, 0),
                    cv::FILLED);
  const double scale = std::max(1.0, dependencies_.config.cropScale);
  const double centerX = (look.box.x + look.box.w / 2.0) * bgr.cols;
  const double centerY = (look.box.y + look.box.h / 2.0) * bgr.rows;
  const int width = std::min(bgr.cols, std::max(kMinCropPx, static_cast<int>(look.box.w * bgr.cols * scale)));
  const int height = std::min(bgr.rows, std::max(kMinCropPx, static_cast<int>(look.box.h * bgr.rows * scale)));
  const int left = std::clamp(static_cast<int>(centerX - width / 2.0), 0, bgr.cols - width);
  const int top = std::clamp(static_cast<int>(centerY - height / 2.0), 0, bgr.rows - height);
  const cv::Mat crop = bgr(cv::Rect(left, top, width, height)).clone();
  std::vector<uint8_t> encoded;
  if (cv::imencode(".jpg", crop, encoded, {cv::IMWRITE_JPEG_QUALITY, kJpegQuality}))
    verdict.jpeg.assign(encoded.begin(), encoded.end());
  if (dependencies_.detector == nullptr || !dependencies_.detector->isLoaded())
    return verdict;
  cv::Mat rgb;
  cv::cvtColor(crop, rgb, cv::COLOR_BGR2RGB);
  for (const DetectedObject& object : dependencies_.detector->detect(
           {.rgb = rgb.data, .width = rgb.cols, .height = rgb.rows})) {
    if (object.name != "person" || object.confidence <= verdict.confidence)
      continue;
    verdict.cls = object.name;
    verdict.confidence = object.confidence;
  }
  return verdict;
}

drogon::Task<SecondLookResult> SecondLook::confirm(SecondLookInput input) const
{
  SecondLookResult result;
  if (dependencies_.source == nullptr)
    co_return result;
  const int64_t deadline = nowMs() + std::max<int64_t>(input.budgetMs, 1);
  const int grabs = std::clamp(input.grabs, 1, 5);
  for (int attempt = 0; attempt < grabs && nowMs() < deadline; ++attempt) {
    auto frame = co_await dependencies_.source->grab({.cameraId = input.cameraId,
                                                      .cameraName = input.cameraName,
                                                      .maxAgeMs = 0,
                                                      .stream = CameraStream::Main});
    if (!frame || frame->jpeg.empty())
      continue;
    result.looked = true;
    result.capturedAtMs = frame->capturedAtMs;
    CropInput crop{.jpeg = std::move(frame->jpeg), .look = input};
    const CropVerdict verdict =
        co_await BlockingTask<CropVerdict>([this, &crop]() { return judge(crop); }, BlockingLane::Heavy);
    if (!verdict.jpeg.empty())
      result.crops.push_back(verdict.jpeg);
    if (verdict.confidence > result.confidence) {
      result.confidence = verdict.confidence;
      result.cls = verdict.cls;
    }
    if (result.confidence >= static_cast<float>(dependencies_.config.confirmConfidence)) {
      result.confirmed = true;
      break;
    }
  }
  LOG_INFO << "Camera second look: camera " << input.cameraId << (result.confirmed ? " confirmed" : " did not confirm")
           << " a person (" << result.confidence << ") over " << result.crops.size() << " main-stream crop(s)";
  co_return result;
}
