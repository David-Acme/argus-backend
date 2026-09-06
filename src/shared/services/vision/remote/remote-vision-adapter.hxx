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

// Parameter struct for the remote describe call surface (AGENTS rule 2) —
// mirrors VisionService::describeMat(Mat, prompt, maxTokens).
struct RemoteDescribeInput
{
  cv::Mat bgr;
  // Empty = the service resolves its own configured default prompt.
  std::string prompt;
  // Per-call max-token steering, as the in-process surface takes it. The
  // frozen describe wire carries no field for it (Ruling BP), so the
  // remote adapter cannot honor it — the service's configured default
  // applies. Steering over the wire is a wire-v2 decision.
  int32_t maxTokens{0};
  // Free-form caller context; the wire service logs it per request.
  std::string cameraId;
};

// Remote stand-in for VisionServiceAdapter (Ruling BQ): same IService
// surface and same describeMat/describeMatAsync call shape, but the Mat is
// encoded to JPEG and described over the argus-vlm wire. The caption cache
// stays local, keyed like the in-process one (FNV of the encoded JPEG +
// prompt) so repeated identical Mats still hit warm state in the legacy.
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

  // The cache key: FNV-1 of the encoded JPEG, folded with the prompt —
  // the same hash shape the in-process service applies to pixels + prompt.
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
