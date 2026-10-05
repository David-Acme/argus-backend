#pragma once

#include <cstdint>
#include <shared/services/face/face-quality.hxx>
#include <string_view>

enum class FaceCheckStatus : std::uint8_t
{
  Accepted,
  Unavailable,
  Undecodable,
  NoFace,
  MultipleFaces,
  PoorQuality,
  SpoofSuspected,
  LivenessUnavailable
};

struct FaceCheckPolicy
{
  FaceQualityGate quality;
  bool livenessRequired{true};
  float livenessThreshold{0.80F};
  bool encodePortrait{false};
};

struct FaceScreenInput
{
  int faces{0};
  const FaceQuality& quality;
  const FaceQualityGate& gate;
};

struct FaceLivenessInput
{
  bool required{true};
  bool engineLoaded{false};
};

namespace face_check
{
inline constexpr float kMinBiometricInterOcularPx = 40.0F;
inline constexpr float kMaxBiometricYaw = 0.25F;

[[nodiscard]] FaceQualityGate biometricGate();
[[nodiscard]] FaceCheckStatus screen(const FaceScreenInput& input);
[[nodiscard]] bool livenessRunnable(const FaceLivenessInput& input);
[[nodiscard]] FaceCheckStatus livenessVerdict(const FaceCheckPolicy& policy,
                                              float realScore);
[[nodiscard]] std::string_view statusToString(FaceCheckStatus status);
}
