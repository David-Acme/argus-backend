#include "remote-vision-adapter.hxx"

#include <shared/services/config-service/config-service.hxx>
#include <shared/services/vision/vision-hash.hxx>
#include <shared/services/vision/remote/vlm-remote.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>

#include <drogon/drogon.h>
#include <opencv2/imgcodecs.hpp>

namespace
{

constexpr int kJpegQuality = 90;
constexpr int kDefaultCacheSlots = 8;

} // namespace

uint64_t RemoteVisionServiceAdapter::cacheKey(std::string_view jpeg,
                                              const std::string& prompt)
{
  return visionHashBytesAndPrompt(
      {.data = reinterpret_cast<const unsigned char*>(jpeg.data()),
       .len = jpeg.size(),
       .prompt = prompt});
}

bool RemoteVisionServiceAdapter::initialize()
{
  config_ = VlmRemoteConfig::resolve();
  if (!config_.enabled())
    return false;

  client_ = std::make_unique<VlmHttpClient>(config_.url, config_.timeoutMs);

  int slots = ConfigService::getInt("vision.caption_cache_slots");
  if (slots <= 0)
    slots = kDefaultCacheSlots;
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.assign(static_cast<size_t>(slots), CacheEntry{});
  cacheNext_ = 0;
  return true;
}

bool RemoteVisionServiceAdapter::isLoaded() const
{
  return client_ != nullptr;
}

void RemoteVisionServiceAdapter::shutdown()
{
  std::lock_guard<std::mutex> lock(mutex_);
  client_.reset();
  cache_.clear();
  cacheNext_ = 0;
}

Json::Value RemoteVisionServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = client_ != nullptr;
  value["remote"] = config_.url;
  return value;
}

const std::string* RemoteVisionServiceAdapter::cacheLookup(uint64_t key)
{
  for (const auto& e : cache_)
    if (e.key == key && !e.caption.empty())
      return &e.caption;
  return nullptr;
}

void RemoteVisionServiceAdapter::cacheStore(uint64_t key,
                                            const std::string& caption)
{
  if (cache_.empty() || caption.empty())
    return;
  cache_[cacheNext_] = CacheEntry{key, caption};
  cacheNext_ = (cacheNext_ + 1) % cache_.size();
}

std::string RemoteVisionServiceAdapter::describeMat(
    const RemoteDescribeInput& input)
{
  if (input.bgr.empty() || !client_)
    return {};

  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<unsigned char> jpeg;
  const std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, kJpegQuality};
  cv::imencode(".jpg", input.bgr, jpeg, params);
  const std::string encoded = drogon::utils::base64Encode(
      jpeg.data(), jpeg.size());

  const uint64_t key = cacheKey(
      std::string_view(reinterpret_cast<const char*>(jpeg.data()),
                       jpeg.size()),
      input.prompt);
  if (const std::string* hit = cacheLookup(key))
    return *hit;

  const std::string caption = client_->describe(
      {.imageJpegB64 = encoded, .prompt = input.prompt,
       .cameraId = input.cameraId});
  cacheStore(key, caption);
  return caption;
}

drogon::Task<std::string> RemoteVisionServiceAdapter::describeMatAsync(
    const RemoteDescribeInput& input)
{
  co_return co_await BlockingTask<std::string>(
      [this, input]() { return describeMat(input); });
}
