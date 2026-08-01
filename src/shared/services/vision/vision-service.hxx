#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <mutex>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <unordered_set>
#include <vector>

struct VisionRequest
{
  std::vector<unsigned char> imageRgb;
  uint32_t width{0};
  uint32_t height{0};
  std::string prompt{"Describe esta imagen de seguridad."};
  int32_t maxTokens{128};
  float temperature{0.3f};
};

class VisionService
{
public:
  VisionService() = delete;
  ~VisionService() = delete;

  static void init();
  static void shutdown();

  static std::string describe(const VisionRequest& req);

  // Coroutine variant: runs inference off the event loop.
  static drogon::Task<std::string> describeAsync(const VisionRequest& req);

  static bool isLoaded();

private:
  // SmolVLM2 chat template prefix with a single image placeholder expanded to
  // <fake_token_around_image><global-img><image>x64<fake_token_around_image>
  // plus the "Can you describe this image?" instruction (pre-tokenized ids).
  static const std::vector<int64_t>& promptIds();

  static std::vector<float> preprocess(const unsigned char* rgb, int width,
                                       int height);
  static std::vector<float> embed(int64_t token);
  static std::vector<std::string>
  decodeTokens(const std::vector<int64_t>& ids);

  static std::unique_ptr<Ort::Session> visionEncoder_;
  static std::unique_ptr<Ort::Session> decoderMerged_;
  static std::unique_ptr<Ort::Session> embedTokens_;
  static Ort::Env env_;
  static std::mutex mutex_;
  static bool loaded_;

  // Frame cache: repeated/near-identical frames (camera feeds) reuse the
  // vision encoder output, skipping the most expensive pass.
  struct FrameCache
  {
    uint64_t hash{0};
    int64_t numTokens{0};
    std::vector<float> features;
  };
  static FrameCache cacheA_;
  static FrameCache cacheB_;
  static int lastCacheSlot_;

  // id -> token string, for detokenizing generated ids.
  static std::vector<std::string> idToToken_;
  // ids of special tokens (skipped when decoding captions).
  static std::unordered_set<int64_t> specialIds_;
};
