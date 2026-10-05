#include "face-quality.hxx"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kReferenceInterOcularPx = 48.0F;
constexpr float kReferenceSharpness = 400.0F;
}

FaceQuality face_quality::geometry(const FaceLandmarks& landmarks)
{
  const float eyeDx = landmarks.rightEyeX - landmarks.leftEyeX;
  const float eyeDy = landmarks.rightEyeY - landmarks.leftEyeY;
  const float interOcular = std::hypot(eyeDx, eyeDy);
  FaceQuality quality;
  quality.interOcularPx = interOcular;
  if (interOcular <= 1.0F)
    return quality;
  const float eyeMidX = (landmarks.leftEyeX + landmarks.rightEyeX) * 0.5F;
  const float eyeMidY = (landmarks.leftEyeY + landmarks.rightEyeY) * 0.5F;
  const float mouthMidX = (landmarks.mouthLeftX + landmarks.mouthRightX) * 0.5F;
  const float mouthMidY = (landmarks.mouthLeftY + landmarks.mouthRightY) * 0.5F;
  const float axisX = mouthMidX - eyeMidX;
  const float axisY = mouthMidY - eyeMidY;
  const float axisLength = std::hypot(axisX, axisY);
  const float lateral =
      ((landmarks.noseX - eyeMidX) * eyeDx + (landmarks.noseY - eyeMidY) * eyeDy) /
      interOcular;
  quality.yaw = std::abs(lateral) / interOcular;
  if (axisLength > 1.0F) {
    const float along =
        ((landmarks.noseX - eyeMidX) * axisX + (landmarks.noseY - eyeMidY) * axisY) /
        axisLength;
    quality.pitch = along / axisLength;
  }
  return quality;
}

FaceQualityVerdict face_quality::judge(const FaceQuality& quality,
                                       const FaceQualityGate& gate)
{
  if (quality.detectorScore < gate.minDetectorScore)
    return FaceQualityVerdict::LowConfidence;
  if (quality.interOcularPx < gate.minInterOcularPx)
    return FaceQualityVerdict::TooSmall;
  if (quality.yaw > gate.maxYaw)
    return FaceQualityVerdict::Turned;
  if (quality.pitch < gate.minPitch || quality.pitch > gate.maxPitch)
    return FaceQualityVerdict::Tilted;
  if (quality.sharpness < gate.minSharpness)
    return FaceQualityVerdict::Blurred;
  return FaceQualityVerdict::Accepted;
}

float face_quality::score(const FaceQuality& quality)
{
  const float size =
      std::clamp(quality.interOcularPx / kReferenceInterOcularPx, 0.0F, 1.0F);
  const float sharp =
      std::clamp(quality.sharpness / kReferenceSharpness, 0.0F, 1.0F);
  const float frontal = std::clamp(1.0F - 2.0F * quality.yaw, 0.0F, 1.0F);
  return std::clamp(quality.detectorScore, 0.0F, 1.0F) *
         (0.4F * size + 0.3F * sharp + 0.3F * frontal);
}

std::string_view face_quality::verdictToString(FaceQualityVerdict verdict)
{
  switch (verdict) {
  case FaceQualityVerdict::Accepted:
    return "accepted";
  case FaceQualityVerdict::LowConfidence:
    return "low_confidence";
  case FaceQualityVerdict::TooSmall:
    return "too_small";
  case FaceQualityVerdict::Turned:
    return "turned";
  case FaceQualityVerdict::Tilted:
    return "tilted";
  case FaceQualityVerdict::Blurred:
    return "blurred";
  }
  return "accepted";
}
