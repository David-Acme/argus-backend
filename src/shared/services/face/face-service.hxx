#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <vector>

namespace ncnn
{
class Net;
class VkBlobAllocator;
class VkStagingAllocator;
class PipelineCache;
} // namespace ncnn

class FaceService
{
public:
  FaceService() = delete;
  ~FaceService() = delete;

  static void init();
  static void shutdown();
  static bool isLoaded();

  struct FaceResult
  {
    std::vector<float> embedding;
    float confidence;
  };

  static std::optional<FaceResult> extract(const uint8_t* rgbData, int width,
                                           int height);

  static std::optional<int64_t> identify(std::string imageBytes);

  // Coroutine variant: runs inference off the event loop.
  static drogon::Task<std::optional<int64_t>>
  identifyAsync(std::string imageBytes);

private:
  static std::counting_semaphore<8> concurrency_;

  struct Impl
  {
    std::unique_ptr<ncnn::VkBlobAllocator> blobAllocator;
    std::unique_ptr<ncnn::VkStagingAllocator> stagingAllocator;
    std::unique_ptr<ncnn::PipelineCache> pipelineCache;
    std::unique_ptr<ncnn::Net> detector;
    std::unique_ptr<ncnn::Net> recognizer;
    bool init(const std::string& modelDir);
  };

  static std::unique_ptr<Impl> impl_;
};
