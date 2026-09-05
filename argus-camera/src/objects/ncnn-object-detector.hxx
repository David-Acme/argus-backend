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

// ncnn implementation of IObjectDetector (YOLO26n). The model input is a
// letterboxed RGB frame at the model's native input size (gray 114 padding)
// and the post-path is selected from the runtime output shape:
// - rows of 6 (e2e graph with in-graph TopK): xyxy + score + cls per row;
// - rows of 4 + class count (raw one2one export): xyxy + per-class scores,
//   TopK is applied here in C++;
// - anything else refuses at load (never silently mis-decoded).
// Vulkan failures fall back to CPU on the same instance; a failed load leaves
// the service disabled (never a held semaphore).
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

  std::vector<DetectedObject> detect(const uint8_t* rgb, int width,
                                     int height) override;

  const std::vector<std::string>& classes() const override
  {
    return options_.classes;
  }

  // "vulkan", "cpu" or "disabled" (model not loaded).
  std::string backend() const;

private:
  // Keeps the net, the blob names read from the .param and the Vulkan
  // allocators alive; replaced (not mutated) when an instance falls back
  // to CPU, so in-flight snapshots keep the old net. implMutex_ guards
  // only that swap — inference runs on a shared snapshot, so per-camera
  // inference overlaps (bounded by the semaphore below).
  struct Impl;

  // Letterbox geometry: scale + padding mapping model boxes to frame pixels.
  struct LetterboxPlan
  {
    float scale{1};
    int padX{0};
    int padY{0};
  };

  std::optional<std::vector<DetectedObject>>
  runNet(Impl& impl, const uint8_t* rgb, int width, int height);
  std::vector<DetectedObject> postProcess(const float* rows, size_t rowCount,
                                          size_t rowLength,
                                          const LetterboxPlan& plan, int width,
                                          int height) const;

  // Loads a fresh net (never mutates a live one: in-flight snapshots keep
  // running on the old instance while a reload swaps the shared pointer).
  static std::shared_ptr<Impl> loadImpl(const std::string& modelDir,
                                        bool useVulkan);

  ObjectDetectorOptions options_;
  mutable std::mutex implMutex_;
  std::counting_semaphore<16> slots_{0};
  std::shared_ptr<Impl> impl_;
};
