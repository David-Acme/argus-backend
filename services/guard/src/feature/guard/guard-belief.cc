#include "guard-belief.hxx"

#include <algorithm>
#include <limits>

namespace guard_belief
{
BeliefResult evaluateBelief(const BeliefInput& input)
{
  BeliefResult result;
  const BeliefConfig& config = input.config;
  const double median = std::clamp(input.scoreMedian, 0.0, 1.0);
  const int samples = std::max(0, input.scoreSamples);
  const int64_t dwellMs = std::max<int64_t>(0, input.dwellMs);
  const int zoneWindows = std::max(0, input.zoneWindows);
  const int trackWindows = std::max(0, input.trackWindows);
  const int64_t trackAgeMs = std::max<int64_t>(0, input.trackAgeMs);
  const double spread = input.areaSpread < 1.0 ? 1.0 : input.areaSpread;
  const int64_t zoneThreshold =
      std::max<int64_t>(1, input.zoneKind == "alert"
                               ? config.zoneDwellAlertMs
                               : config.zoneDwellMonitorMs);
  const auto apply = [&result](BeliefSignal signal, int weight) {
    result.signals.push_back(signal);
    result.score += weight;
  };

  if (samples >= config.minScoreSamples && median >= config.detectorStrong)
    apply(BeliefSignal::DetectorStrong, config.weightDetectorStrong);
  else if (samples >= config.minScoreSamples && median < config.detectorWeak)
    apply(BeliefSignal::DetectorWeak, config.weightDetectorWeak);

  if (zoneWindows >= config.persistenceWindows && dwellMs >= zoneThreshold)
    apply(BeliefSignal::PersistenceMet, config.weightPersistenceMet);
  else if (dwellMs <
           static_cast<int64_t>(config.persistenceDwellFraction * zoneThreshold))
    apply(BeliefSignal::PersistenceShort, config.weightPersistenceShort);

  if (trackAgeMs >= config.trackStableAgeMs && trackWindows >= 2)
    apply(BeliefSignal::TrackStable, config.weightTrackStable);
  if (trackWindows < 2 || dwellMs < config.trackJitterDwellMs ||
      spread > config.areaSpreadRatio)
    apply(BeliefSignal::TrackJitter, config.weightTrackJitter);

  if (!input.identityAvailable)
    apply(BeliefSignal::IdentityUnavailable,
          config.weightIdentityUnavailable);
  else if (input.identity == IdentityState::Unrecognized)
    apply(BeliefSignal::IdentityUnrecognized, config.weightIdentityUnrecognized);
  else if (input.identity == IdentityState::Unobservable)
    apply(BeliefSignal::IdentityUnobservable, config.weightIdentityUnobservable);
  else
    apply(BeliefSignal::IdentityKnown, config.weightIdentityKnown);

  if (input.healthDegraded)
    apply(BeliefSignal::CameraHealthDegraded,
          config.weightCameraHealthDegraded);

  return result;
}

int beliefThreshold(GuardDanger severity, const BeliefConfig& config)
{
  switch (severity) {
  case GuardDanger::Critical:
    return config.thresholdCritical;
  case GuardDanger::High:
    return config.thresholdHigh;
  case GuardDanger::Medium:
    return config.thresholdMedium;
  case GuardDanger::Low:
    return config.thresholdLow;
  case GuardDanger::None:
    return std::numeric_limits<int>::max();
  }
  return std::numeric_limits<int>::max();
}

bool beliefSuppressesKind(const GateScopeInput& input)
{
  switch (input.kind) {
  case GuardActionKind::Notify:
    return true;
  case GuardActionKind::Announce:
    return input.scope == BeliefGateScope::Communication ||
           input.scope == BeliefGateScope::All;
  case GuardActionKind::Alarm:
  case GuardActionKind::SirenArm:
    return input.scope == BeliefGateScope::All && !input.hardFloor;
  default:
    return false;
  }
}
}
