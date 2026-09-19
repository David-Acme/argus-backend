#pragma once

#include <guard-policy.hxx>
#include <shared/enums.hxx>

#include <optional>
#include <string>
#include <vector>

// Closed observable belief vocabulary; no model input ever reaches this gate.
enum class BeliefSignal
{
  DetectorStrong,
  DetectorWeak,
  PersistenceMet,
  PersistenceShort,
  TrackStable,
  TrackJitter,
  IdentityUnrecognized,
  IdentityUnobservable,
  IdentityUnavailable,
  IdentityKnown,
  CameraHealthDegraded,
};

inline std::string beliefSignalToString(BeliefSignal signal)
{
  switch (signal) {
  case BeliefSignal::DetectorStrong:
    return "detector_strong";
  case BeliefSignal::DetectorWeak:
    return "detector_weak";
  case BeliefSignal::PersistenceMet:
    return "persistence_met";
  case BeliefSignal::PersistenceShort:
    return "persistence_short";
  case BeliefSignal::TrackStable:
    return "track_stable";
  case BeliefSignal::TrackJitter:
    return "track_jitter";
  case BeliefSignal::IdentityUnrecognized:
    return "identity_unrecognized";
  case BeliefSignal::IdentityUnobservable:
    return "identity_unobservable";
  case BeliefSignal::IdentityUnavailable:
    return "identity_unavailable";
  case BeliefSignal::IdentityKnown:
    return "identity_known";
  case BeliefSignal::CameraHealthDegraded:
    return "camera_health_degraded";
  }
  return "camera_health_degraded";
}

// Resolved weights and thresholds; the struct keeps the engine pure.
struct BeliefConfig
{
  int weightDetectorStrong{2};
  int weightDetectorWeak{-2};
  int weightPersistenceMet{2};
  int weightPersistenceShort{-2};
  int weightTrackStable{1};
  int weightTrackJitter{-2};
  int weightIdentityUnrecognized{2};
  int weightIdentityUnobservable{-2};
  int weightIdentityUnavailable{0};
  int weightIdentityKnown{-3};
  int weightCameraHealthDegraded{-2};
  double detectorStrong{0.75};
  double detectorWeak{0.35};
  int minScoreSamples{1};
  int persistenceWindows{2};
  double persistenceDwellFraction{0.5};
  int64_t zoneDwellAlertMs{3000};
  int64_t zoneDwellMonitorMs{12000};
  int64_t trackStableAgeMs{4000};
  int64_t trackJitterDwellMs{1500};
  double areaSpreadRatio{2.0};
  int thresholdCritical{1};
  int thresholdHigh{3};
  int thresholdMedium{5};
  int thresholdLow{7};
};

struct BeliefInput
{
  double scoreMedian{0.0};
  int scoreSamples{0};
  int64_t dwellMs{0};
  int zoneWindows{0};
  int trackWindows{0};
  std::string zoneKind;
  int64_t trackAgeMs{0};
  double areaSpread{1.0};
  IdentityState identity{IdentityState::Unrecognized};
  bool identityAvailable{true};
  bool healthDegraded{false};
  BeliefConfig config;
};

struct BeliefResult
{
  int score{0};
  std::vector<BeliefSignal> signals;
};

// Which effect kinds the belief gate may suppress in enforce mode.
enum class BeliefGateScope
{
  Notify,
  Communication,
  All,
};

inline std::optional<BeliefGateScope>
beliefGateScopeFromString(const std::string& value)
{
  if (value == "notify")
    return BeliefGateScope::Notify;
  if (value == "communication")
    return BeliefGateScope::Communication;
  if (value == "all")
    return BeliefGateScope::All;
  return std::nullopt;
}

inline std::string beliefGateScopeToString(BeliefGateScope scope)
{
  switch (scope) {
  case BeliefGateScope::Notify:
    return "notify";
  case BeliefGateScope::Communication:
    return "communication";
  case BeliefGateScope::All:
    return "all";
  }
  return "notify";
}

struct GateScopeInput
{
  BeliefGateScope scope{BeliefGateScope::Notify};
  GuardActionKind kind{GuardActionKind::Notify};
  bool hardFloor{false};
};

// Human phrasing for one wire signal name, shared by the notification body
// and the decision-journal read path so both render identical reasons.
inline std::string beliefSignalPhrase(const std::string& signal)
{
  if (signal == "detector_strong")
    return "strong detection";
  if (signal == "detector_weak")
    return "weak detection";
  if (signal == "persistence_met")
    return "lingering";
  if (signal == "persistence_short")
    return "brief presence";
  if (signal == "track_stable")
    return "steady track";
  if (signal == "track_jitter")
    return "unstable track";
  if (signal == "camera_health_degraded")
    return "poor visibility";
  if (signal == "identity_unrecognized")
    return "unrecognized person";
  if (signal == "identity_unobservable")
    return "unidentified person";
  if (signal == "identity_known")
    return "known person";
  return {};
}

namespace guard_belief
{
// Pure belief evaluation over resolved inputs; no database, no NATS, no model.
BeliefResult evaluateBelief(const BeliefInput& input);

// Asymmetric Bayes-risk threshold: critical demands the least belief.
int beliefThreshold(GuardDanger severity, const BeliefConfig& config);

// Resolves globals then per-camera overrides for one camera.
BeliefConfig resolveBeliefConfig(int64_t cameraId);

// Whether the belief gate may suppress one effect kind. Notify and announce
// follow the scope; alarm and siren-arm additionally require no hard floor,
// so a hard floor always lets physical effects through.
bool beliefSuppressesKind(const GateScopeInput& input);
} // namespace guard_belief
