#pragma once

#include <objects/object-detector.hxx>

#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <vector>

namespace ncnn
{
class Net;
}

// ncnn implementation of IObjectDetector (YOLO26n), letterboxed RGB input.
struct ObjectDetectorOptions
{
  std::string modelDir;
  std::vector<std::string> classes;
  int inputSize{640};
  float confidence{0.45f};
  int maxDet{300};
  bool useVulkan{true};
};

class ObjectDetectorService final : public IObjectDetector
{
public:
  explicit ObjectDetectorService(ObjectDetectorOptions options);
  ~ObjectDetectorService() override;

  ObjectDetectorService(const ObjectDetectorService&) = delete;
  ObjectDetectorService& operator=(const ObjectDetectorService&) = delete;

  void init();
  bool isLoaded() const override;

  std::vector<DetectedObject> detect(const DetectInput& input) override;

  const std::vector<std::string>& classes() const override
  {
    return options_.classes;
  }

  // "vulkan", "cpu" or "disabled" (model not loaded).
  std::string backend() const;

private:
  // Keeps the net and Vulkan allocators alive; replaced on CPU fallback.
  struct Impl;

  // Letterbox geometry: scale + padding mapping model boxes to frame pixels.
  struct LetterboxPlan
  {
    float scale{1};
    int padX{0};
    int padY{0};
  };

  struct RunNetInput
  {
    Impl& impl;
    const uint8_t* rgb{nullptr};
    int width{0};
    int height{0};
  };

  struct PostProcessInput
  {
    const float* rows{nullptr};
    size_t rowCount{0};
    size_t rowLength{0};
    const LetterboxPlan& plan;
    int width{0};
    int height{0};
  };

  std::optional<std::vector<DetectedObject>>
  runNet(const RunNetInput& input);
  std::vector<DetectedObject>
  postProcess(const PostProcessInput& input) const;

  // Loads a fresh net; in-flight snapshots keep running on the old instance.
  static std::shared_ptr<Impl> loadImpl(const std::string& modelDir,
                                        bool useVulkan);

  ObjectDetectorOptions options_;
  mutable std::mutex implMutex_;
  std::counting_semaphore<16> slots_{0};
  std::shared_ptr<Impl> impl_;
};
