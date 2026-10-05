#include "face-check.hxx"

#include <shared/services/face/anti-spoof.hxx>

namespace
{
constexpr float kMinBiometricDetectorScore = 0.80F;
constexpr float kMinBiometricPitch = 0.25F;
constexpr float kMaxBiometricPitch = 0.85F;
constexpr float kMinBiometricSharpness = 20.0F;
}

FaceQualityGate face_check::biometricGate()
{
  return FaceQualityGate{.minDetectorScore = kMinBiometricDetectorScore,
                         .minInterOcularPx = kMinBiometricInterOcularPx,
                         .maxYaw = kMaxBiometricYaw,
                         .minPitch = kMinBiometricPitch,
                         .maxPitch = kMaxBiometricPitch,
                         .minSharpness = kMinBiometricSharpness};
}

FaceCheckStatus face_check::screen(const FaceScreenInput& input)
{
  if (input.faces <= 0)
    return FaceCheckStatus::NoFace;
  if (input.faces > 1)
    return FaceCheckStatus::MultipleFaces;
  if (face_quality::judge(input.quality, input.gate) != FaceQualityVerdict::Accepted)
    return FaceCheckStatus::PoorQuality;
  return FaceCheckStatus::Accepted;
}

bool face_check::livenessRunnable(const FaceLivenessInput& input)
{
  return !input.required || input.engineLoaded;
}

FaceCheckStatus face_check::livenessVerdict(const FaceCheckPolicy& policy,
                                            float realScore)
{
  if (!policy.livenessRequired)
    return FaceCheckStatus::Accepted;
  return anti_spoof::isLive(realScore, policy.livenessThreshold)
             ? FaceCheckStatus::Accepted
             : FaceCheckStatus::SpoofSuspected;
}

std::string_view face_check::statusToString(FaceCheckStatus status)
{
  switch (status) {
  case FaceCheckStatus::Accepted:
    return "accepted";
  case FaceCheckStatus::Unavailable:
    return "unavailable";
  case FaceCheckStatus::Undecodable:
    return "undecodable";
  case FaceCheckStatus::NoFace:
    return "no_face";
  case FaceCheckStatus::MultipleFaces:
    return "multiple_faces";
  case FaceCheckStatus::PoorQuality:
    return "poor_quality";
  case FaceCheckStatus::SpoofSuspected:
    return "liveness_failed";
  case FaceCheckStatus::LivenessUnavailable:
    return "liveness_unavailable";
  }
  return "unavailable";
}
