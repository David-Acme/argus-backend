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

struct VisionDefaults
{
  int32_t maxInputPx{384};
  int32_t maxTokens{64};
  std::string prompt;
  int32_t cacheSlots{8};
};

struct VisionEngineSettings
{
  int32_t threads{0};
  int32_t gpuLayers{-1};
  int32_t contextSize{8192};
  int32_t imageMaxTokens{0};
};

[[nodiscard]] VisionDefaults resolveVisionDefaults();

[[nodiscard]] VisionEngineSettings resolveVisionEngineSettings();

class VisionService
{
public:
  VisionService();
  ~VisionService();

  VisionService(const VisionService&) = delete;
  VisionService& operator=(const VisionService&) = delete;

  void init();
  void shutdown();
  void refreshDefaults();

  std::string describe(const VisionRequest& req);
  std::string describeMat(const VisionDescribeMatInput& input);

  drogon::Task<std::string> describeAsync(const VisionRequest& req);
  drogon::Task<std::string> describeMatAsync(const VisionDescribeMatInput& input);

  void cancel();
  [[nodiscard]] bool isLoaded() const;
  [[nodiscard]] int maxInputPx() const { return maxInputPx_.load(std::memory_order_relaxed); }
  [[nodiscard]] int defaultMaxTokens() const { return defaultMaxTokens_.load(std::memory_order_relaxed); }

private:
  void loadDefaults();
  std::string run(const VisionRunInput& input);
  cv::Mat fitToBudget(const cv::Mat& src, bool srcIsBgr);
  const std::string* cacheLookup(uint64_t key);
  void cacheStore(uint64_t key, const std::string& caption);

  std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
  std::unique_ptr<llama_context, void (*)(llama_context*)> context_;
  std::unique_ptr<mtmd_context, void (*)(mtmd_context*)> mtmd_;

  std::mutex mutex_;
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> loaded_{false};

  std::atomic<int32_t> defaultMaxTokens_{64};
  std::atomic<int32_t> maxInputPx_{384};
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
