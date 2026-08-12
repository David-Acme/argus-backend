#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <optional>
#include <semaphore>
#include <shared/services/face/face-db.hxx>
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
  FaceService();
  ~FaceService();

  FaceService(const FaceService&) = delete;
  FaceService& operator=(const FaceService&) = delete;

  static FaceService& instance();

  void init();
  void shutdown();
  bool isLoaded() const;

  struct FaceResult
  {
    std::vector<float> embedding;
    float confidence;
  };

  std::optional<FaceResult> extract(const uint8_t* rgbData, int width,
                                    int height);

  std::optional<int64_t> identify(std::string imageBytes);

  // Coroutine variant: runs inference off the event loop.
  drogon::Task<std::optional<int64_t>> identifyAsync(std::string imageBytes);

  FaceDB& faceDb() { return faceDb_; }

private:
  std::counting_semaphore<8> concurrency_{0};

  struct Impl
  {
    std::unique_ptr<ncnn::VkBlobAllocator> blobAllocator;
    std::unique_ptr<ncnn::VkStagingAllocator> stagingAllocator;
    std::unique_ptr<ncnn::PipelineCache> pipelineCache;
    std::unique_ptr<ncnn::Net> detector;
    std::unique_ptr<ncnn::Net> recognizer;
    bool init(const std::string& modelDir);
  };

  std::unique_ptr<Impl> impl_;
  FaceDB faceDb_;
};
