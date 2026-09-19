#pragma once

#include <config/service.hxx>
#include <shared/services/vision/remote/vlm-remote.hxx>

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <opencv2/core.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Remote describe call surface mirroring VisionService::describeMat(Mat, prompt, maxTokens).
struct RemoteDescribeInput
{
  cv::Mat bgr;
  std::string prompt;
  int32_t maxTokens{0};
  std::string cameraId;
};

// Remote stand-in for VisionServiceAdapter: describe over the argus-vlm wire, local caption cache.
class RemoteVisionServiceAdapter : public IService
{
public:
  std::string name() const override { return "vision"; }
  std::string version() const override { return "1.0.0"; }
  bool initialize() override;
  bool isLoaded() const override;
  void shutdown() override;
  Json::Value health() const override;

  std::string describeMat(const RemoteDescribeInput& input);
  drogon::Task<std::string>
  describeMatAsync(const RemoteDescribeInput& input);

  // Cache key: FNV-1 of the encoded JPEG folded with the prompt.
  static uint64_t cacheKey(std::string_view jpeg, const std::string& prompt);

private:
  struct CacheEntry
  {
    uint64_t key{0};
    std::string caption;
  };
  const std::string* cacheLookup(uint64_t key);
  void cacheStore(uint64_t key, const std::string& caption);

  VlmRemoteConfig config_;
  std::unique_ptr<VlmHttpClient> client_;
  std::mutex mutex_;
  std::vector<CacheEntry> cache_;
  size_t cacheNext_ = 0;
};
