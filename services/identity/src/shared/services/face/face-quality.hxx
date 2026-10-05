#pragma once

#include <cstdint>
#include <string_view>

struct FaceLandmarks
{
  float leftEyeX{0.0F};
  float leftEyeY{0.0F};
  float rightEyeX{0.0F};
  float rightEyeY{0.0F};
  float noseX{0.0F};
  float noseY{0.0F};
  float mouthLeftX{0.0F};
  float mouthLeftY{0.0F};
  float mouthRightX{0.0F};
  float mouthRightY{0.0F};
};

struct FaceQuality
{
  float detectorScore{0.0F};
  float faceWidthPx{0.0F};
  float interOcularPx{0.0F};
  float yaw{0.0F};
  float pitch{0.0F};
  float sharpness{0.0F};
};

struct FaceQualityGate
{
  float minDetectorScore{0.0F};
  float minInterOcularPx{0.0F};
  float maxYaw{0.0F};
  float minPitch{0.0F};
  float maxPitch{0.0F};
  float minSharpness{0.0F};
};

enum class FaceQualityVerdict : std::uint8_t
{
  Accepted,
  LowConfidence,
  TooSmall,
  Turned,
  Tilted,
  Blurred
};

namespace face_quality
{
[[nodiscard]] FaceQuality geometry(const FaceLandmarks& landmarks);
[[nodiscard]] FaceQualityVerdict judge(const FaceQuality& quality,
                                       const FaceQualityGate& gate);
[[nodiscard]] float score(const FaceQuality& quality);
[[nodiscard]] std::string_view verdictToString(FaceQualityVerdict verdict);
}
