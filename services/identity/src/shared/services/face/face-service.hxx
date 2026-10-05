#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <optional>
#include <shared/services/face/anti-spoof.hxx>
#include <shared/services/face/face-check.hxx>
#include <shared/services/face/face-db.hxx>
#include <shared/services/face/face-image.hxx>
#include <shared/services/face/face-quality.hxx>
#include <shared/services/face/inference-slots.hxx>
#include <shared_mutex>
#include <string>
#include <vector>

namespace ncnn
{
class Net;
class PipelineCache;
}

class FaceService
{
public:
  FaceService();
  ~FaceService();

  FaceService(const FaceService&) = delete;
  FaceService& operator=(const FaceService&) = delete;

  static FaceService& instance();

  void init();
  void init(const std::string& modelDir);
  AntiSpoofLoad initLiveness(const std::string& modelDir);
  void disable();
  void shutdown();
  bool isLoaded() const;
  bool livenessLoaded() const;

  struct FaceResult
  {
    std::vector<float> embedding;
    float confidence;
  };

  struct FaceBox
  {
    float x1{0.0F};
    float y1{0.0F};
    float x2{0.0F};
    float y2{0.0F};
    float score{0.0F};
    std::array<float, 10> lm{};
  };

  struct ExtractInput
  {
    const uint8_t* rgbData{nullptr};
    int width{0};
    int height{0};
  };

  struct AlignFaceInput
  {
    const uint8_t* rgbData{nullptr};
    int width{0};
    int height{0};
    const float* landmarks{nullptr};
  };

  static constexpr int kAlignedSide = 112;

  static std::vector<uint8_t> alignFace(const AlignFaceInput& input);

  struct FaceAnalysis
  {
    std::vector<float> embedding;
    FaceBox box;
    FaceQuality quality;
    int faces{0};
    std::string faceJpeg;
  };

  struct AnalyzeImageInput
  {
    std::string imageBytes;
    bool encodeFace{false};
  };

  std::optional<FaceAnalysis> analyzeImage(const AnalyzeImageInput& input);
  drogon::Task<std::optional<FaceAnalysis>>
  analyzeImageAsync(AnalyzeImageInput input);

  std::optional<FaceResult> extractImage(std::string imageBytes);
  drogon::Task<std::optional<FaceResult>>
  extractImageAsync(std::string imageBytes);

  struct FaceCheck
  {
    FaceCheckStatus status{FaceCheckStatus::Unavailable};
    std::vector<float> embedding;
    FaceQuality quality;
    float confidence{0.0F};
    std::optional<float> liveness;
    std::string portraitJpeg;
  };

  struct VerifyImageInput
  {
    std::string imageBytes;
    FaceCheckPolicy policy;
  };

  FaceCheck verifyImage(const VerifyImageInput& input);
  drogon::Task<FaceCheck> verifyImageAsync(VerifyImageInput input);

  FaceDB& faceDb() { return faceDb_; }

private:
  struct Impl
  {
    std::unique_ptr<ncnn::PipelineCache> pipelineCache;
    std::unique_ptr<ncnn::Net> detector;
    std::unique_ptr<ncnn::Net> recognizer;
    bool init(const std::string& modelDir);
  };

  struct RunDetectorInput
  {
    const Impl& impl;
    const uint8_t* rgbData{nullptr};
    int width{0};
    int height{0};
  };

  static std::vector<FaceBox> runDetector(const RunDetectorInput& input);

  struct EmbedBoxInput
  {
    const Impl& impl;
    const uint8_t* rgbData{nullptr};
    int width{0};
    int height{0};
    const FaceBox& box;
    bool encodeFace{false};
  };

  static FaceAnalysis embedBox(const EmbedBoxInput& input);
  std::optional<FaceAnalysis> analyzePixels(const ExtractInput& input,
                                            bool encodeFace);
  std::optional<FaceAnalysis> analyzeBytes(const AnalyzeImageInput& input);
  FaceCheck verifyBytes(const VerifyImageInput& input);

  InferenceSlots slots_;
  std::atomic<bool> disabled_{false};
  mutable std::shared_mutex implMutex_;
  std::unique_ptr<Impl> impl_;
  AntiSpoofEngine antiSpoof_;
  FaceDB faceDb_;
};
