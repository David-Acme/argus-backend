#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

struct llama_model;
struct llama_context;
struct mtmd_context;

// Describe request; empty prompt and maxTokens 0 keep the configured defaults.
struct VisionRequest
{
  std::vector<unsigned char> imageRgb;
  uint32_t width{0};
  uint32_t height{0};
  std::string prompt;
  int32_t maxTokens{0};
};

struct VisionDescribeMatInput
{
  cv::Mat bgr;
  std::string prompt;
  int32_t maxTokens{0};
};

struct VisionRunInput
{
  cv::Mat src;
  bool srcIsBgr{false};
  std::string prompt;
  int32_t maxTokens{0};
};

class VisionService
{
public:
  VisionService();
  ~VisionService();

  VisionService(const VisionService&) = delete;
  VisionService& operator=(const VisionService&) = delete;

  void init();
  void shutdown();

  std::string describe(const VisionRequest& req);
  std::string describeMat(const VisionDescribeMatInput& input);

  // Coroutine variants: run inference off the event loop.
  drogon::Task<std::string> describeAsync(const VisionRequest& req);
  drogon::Task<std::string> describeMatAsync(const VisionDescribeMatInput& input);

  void cancel();
  bool isLoaded() const;
  int maxInputPx() const { return maxInputPx_; }
  int defaultMaxTokens() const { return defaultMaxTokens_; }

private:
  std::string run(const VisionRunInput& input);
  cv::Mat fitToBudget(const cv::Mat& src, bool srcIsBgr);
  const std::string* cacheLookup(uint64_t key);
  void cacheStore(uint64_t key, const std::string& caption);

  std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
  std::unique_ptr<llama_context, void (*)(llama_context*)> context_;
  std::unique_ptr<mtmd_context, void (*)(mtmd_context*)> mtmd_;

  std::mutex mutex_;
  std::atomic<bool> cancelled_{false};
  bool loaded_ = false;

  int32_t defaultMaxTokens_ = 64;
  int32_t maxInputPx_ = 384;
  int32_t nBatch_ = 512;
  std::string defaultPrompt_;

  struct CacheEntry
  {
    uint64_t key{0};
    std::string caption;
  };
  std::vector<CacheEntry> cache_;
  size_t cacheNext_ = 0;
};
