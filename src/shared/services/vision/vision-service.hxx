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
  // Empty = use the configured default question.
  std::string prompt;
  // 0 = use the configured default (vision.max_tokens).
  int32_t maxTokens{0};
};

class VisionService
{
public:
  VisionService() = delete;
  ~VisionService() = delete;

  static void init();
  static void shutdown();

  static std::string describe(const VisionRequest& req);
  static std::string describeMat(const cv::Mat& bgr,
                                 const std::string& prompt = {},
                                 int32_t maxTokens = 0);

  // Coroutine variants: run inference off the event loop.
  static drogon::Task<std::string> describeAsync(const VisionRequest& req);
  static drogon::Task<std::string> describeMatAsync(const cv::Mat& bgr,
                                                    const std::string& prompt = {},
                                                    int32_t maxTokens = 0);

  static void cancel();
  static bool isLoaded();

private:
  static std::string run(const cv::Mat& src, bool srcIsBgr,
                         const std::string& prompt, int32_t maxTokens);
  static cv::Mat fitToBudget(const cv::Mat& src, bool srcIsBgr);
  static const std::string* cacheLookup(uint64_t key);
  static void cacheStore(uint64_t key, const std::string& caption);

  static std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
  static std::unique_ptr<llama_context, void (*)(llama_context*)> context_;
  static std::unique_ptr<mtmd_context, void (*)(mtmd_context*)> mtmd_;

  static std::mutex mutex_;
  static std::atomic<bool> cancelled_;
  static bool loaded_;

  static int32_t defaultMaxTokens_;
  static int32_t maxInputPx_;
  static int32_t nBatch_;
  static std::string defaultPrompt_;

  struct CacheEntry
  {
    uint64_t key{0};
    std::string caption;
  };
  static std::vector<CacheEntry> cache_;
  static size_t cacheNext_;
};
