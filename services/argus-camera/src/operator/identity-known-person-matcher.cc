#include <operator/identity-known-person-matcher.hxx>

#include <identity/identity-client.hxx>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <shared/services/stream/snapshot-store.hxx>

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct CropRect
{
  int x{0};
  int y{0};
  int width{0};
  int height{0};
};

CropRect cropRect(const PersonCrop& crop)
{
  const int x =
      std::clamp(static_cast<int>(crop.x), 0, std::max(0, crop.width - 1));
  const int y =
      std::clamp(static_cast<int>(crop.y), 0, std::max(0, crop.height - 1));
  return {.x = x,
          .y = y,
          .width = std::clamp(static_cast<int>(crop.w), 1, crop.width - x),
          .height = std::clamp(static_cast<int>(crop.h), 1, crop.height - y)};
}
} // namespace

IdentityKnownPersonMatcher::IdentityKnownPersonMatcher(IdentityConfig config)
    : config_(std::move(config)),
      client_(
          std::make_unique<IdentityClient>(config_.target, config_.rpcSecret))
{
}

IdentityKnownPersonMatcher::~IdentityKnownPersonMatcher() = default;

std::optional<PersonMatch> IdentityKnownPersonMatcher::match(
    const PersonCrop& crop) const
{
  if (!config_.identify || crop.rgb == nullptr ||
      crop.w < config_.minFaceBoxPx || crop.h < config_.minFaceBoxPx)
    return std::nullopt;

  const int64_t stamp = nowMs();
  const double score = cropQuality(crop);
  const std::pair<int64_t, int64_t> key{crop.cameraId, crop.trackId};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.size() > 256) {
      for (auto it = cache_.begin(); it != cache_.end();) {
        if (stamp - it->second.firstMs >= config_.bestShotMs * 4 &&
            !(it->first == key))
          it = cache_.erase(it);
        else
          ++it;
      }
    }
    auto found = cache_.find(key);
    if (found != cache_.end() &&
        stamp - found->second.firstMs >= config_.bestShotMs)
      found = cache_.erase(found);
    if (found != cache_.end()) {
      const CacheEntry& entry = found->second;
      if (entry.result && entry.result->identity == PersonIdentity::Known)
        return entry.result;
      const bool improved = entry.score <= 0.0 ||
                            score > entry.score * (1.0 + config_.improveMargin);
      if (!improved && (entry.scanned ||
                        stamp - entry.lastScanMs < config_.identifyIntervalMs))
        return entry.result;
    }
  }

  const std::string image = encodeCrop(crop);
  if (!image.empty())
    SnapshotStore::instance().putPersonCrop(crop.cameraId, crop.trackId, image,
                                            stamp);
  std::optional<PersonMatch> result;
  if (!image.empty()) {
    const auto identified = client_->identifyPerson(image);
    if (identified && identified->matched() && identified->person_id() > 0) {
      result = PersonMatch{.identity = identified->trusted()
                                           ? PersonIdentity::Known
                                           : PersonIdentity::Unknown,
                           .personId = identified->person_id(),
                           .confidence = identified->confidence()};
    }
    else if (config_.autoEnroll && canEnroll(crop.cameraId, stamp)) {
      const auto enrolled = client_->enrollPerson(
          {.image = image,
           .cameraId = crop.cameraId,
           .captureSnapshot = config_.captureClearFaces});
      if (enrolled && enrolled->person_id() > 0) {
        result = PersonMatch{
            .identity = enrolled->created() ? PersonIdentity::Unknown
                                            : PersonIdentity::Known,
            .personId = enrolled->person_id(),
            .confidence = enrolled->confidence()};
        std::lock_guard<std::mutex> lock(mutex_);
        lastEnrollMs_[crop.cameraId] = stamp;
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheEntry& entry = cache_[key];
    if (entry.firstMs == 0)
      entry.firstMs = stamp;
    const bool better = score >= entry.score;
    if (better || !entry.scanned) {
      entry.score = score;
      entry.result = result;
    }
    entry.lastScanMs = stamp;
    entry.scanned = true;
  }
  return result;
}

std::string IdentityKnownPersonMatcher::encodeCrop(const PersonCrop& crop) const
{
  const CropRect rect = cropRect(crop);
  if (rect.width <= 1 || rect.height <= 1)
    return {};

  const cv::Mat rgb(crop.height, crop.width, CV_8UC3,
                    const_cast<uint8_t*>(crop.rgb));
  cv::Mat bgr;
  cv::cvtColor(rgb(cv::Rect(rect.x, rect.y, rect.width, rect.height)), bgr,
               cv::COLOR_RGB2BGR);
  std::vector<uchar> buffer;
  if (!cv::imencode(".jpg", bgr, buffer, {cv::IMWRITE_JPEG_QUALITY, 85}))
    return {};
  return std::string(buffer.begin(), buffer.end());
}

double IdentityKnownPersonMatcher::cropQuality(const PersonCrop& crop) const
{
  const CropRect rect = cropRect(crop);
  if (rect.width < 8 || rect.height < 8)
    return 0.0;

  const cv::Mat rgb(crop.height, crop.width, CV_8UC3,
                    const_cast<uint8_t*>(crop.rgb));
  cv::Mat gray;
  cv::cvtColor(rgb(cv::Rect(rect.x, rect.y, rect.width, rect.height)), gray,
               cv::COLOR_RGB2GRAY);
  cv::Mat laplacian;
  cv::Laplacian(gray, laplacian, CV_64F);
  cv::Scalar mean;
  cv::Scalar stddev;
  cv::meanStdDev(laplacian, mean, stddev);
  const double sharpness = std::max(1.0, stddev[0] * stddev[0]);
  return static_cast<double>(crop.w) * crop.h * sharpness;
}

bool IdentityKnownPersonMatcher::canEnroll(int64_t cameraId,
                                           int64_t stamp) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto last = lastEnrollMs_.find(cameraId);
  return last == lastEnrollMs_.end() ||
         stamp - last->second >= config_.enrollCooldownMs;
}
