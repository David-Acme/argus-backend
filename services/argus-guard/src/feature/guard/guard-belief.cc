#include <guard-belief.hxx>

#include <shared/services/config-service/config-service.hxx>

#include <algorithm>
#include <limits>

namespace
{
int configIntOr(const std::string& key, int fallback)
{
  if (!ConfigService::hasKey(key))
    return fallback;
  return ConfigService::getInt(key);
}

int64_t configInt64Or(const std::string& key, int64_t fallback)
{
  if (!ConfigService::hasKey(key))
    return fallback;
  return static_cast<int64_t>(ConfigService::getInt(key));
}

double configDoubleOr(const std::string& key, double fallback)
{
  if (!ConfigService::hasKey(key))
    return fallback;
  return ConfigService::getDouble(key);
}

std::string cameraLeafKey(int64_t cameraId, const std::string& leaf)
{
  return "guard.belief.camera." + std::to_string(cameraId) + "." + leaf;
}

// Effective key for one leaf: the per-camera override wins when present.
std::string beliefLeafKey(int64_t cameraId, const std::string& leaf)
{
  const std::string overrideKey = cameraLeafKey(cameraId, leaf);
  if (ConfigService::hasKey(overrideKey))
    return overrideKey;
  return "guard.belief." + leaf;
}
} // namespace

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

BeliefConfig resolveBeliefConfig(int64_t cameraId)
{
  const BeliefConfig defaults;
  BeliefConfig config;
  config.weightDetectorStrong = configIntOr(
      beliefLeafKey(cameraId, "weight_detector_strong"),
      defaults.weightDetectorStrong);
  config.weightDetectorWeak = configIntOr(
      beliefLeafKey(cameraId, "weight_detector_weak"),
      defaults.weightDetectorWeak);
  config.weightPersistenceMet = configIntOr(
      beliefLeafKey(cameraId, "weight_persistence_met"),
      defaults.weightPersistenceMet);
  config.weightPersistenceShort = configIntOr(
      beliefLeafKey(cameraId, "weight_persistence_short"),
      defaults.weightPersistenceShort);
  config.weightTrackStable = configIntOr(
      beliefLeafKey(cameraId, "weight_track_stable"),
      defaults.weightTrackStable);
  config.weightTrackJitter = configIntOr(
      beliefLeafKey(cameraId, "weight_track_jitter"),
      defaults.weightTrackJitter);
  config.weightIdentityUnrecognized = configIntOr(
      beliefLeafKey(cameraId, "weight_identity_unrecognized"),
      defaults.weightIdentityUnrecognized);
  config.weightIdentityUnobservable = configIntOr(
      beliefLeafKey(cameraId, "weight_identity_unobservable"),
      defaults.weightIdentityUnobservable);
  config.weightIdentityUnavailable = configIntOr(
      beliefLeafKey(cameraId, "weight_identity_unavailable"),
      defaults.weightIdentityUnavailable);
  config.weightIdentityKnown = configIntOr(
      beliefLeafKey(cameraId, "weight_identity_known"),
      defaults.weightIdentityKnown);
  config.weightCameraHealthDegraded = configIntOr(
      beliefLeafKey(cameraId, "weight_camera_health_degraded"),
      defaults.weightCameraHealthDegraded);
  config.detectorStrong = configDoubleOr(
      beliefLeafKey(cameraId, "detector_strong"), defaults.detectorStrong);
  config.detectorWeak = configDoubleOr(
      beliefLeafKey(cameraId, "detector_weak"), defaults.detectorWeak);
  config.minScoreSamples = configIntOr(
      beliefLeafKey(cameraId, "min_score_samples"), defaults.minScoreSamples);
  config.persistenceWindows = configIntOr(
      beliefLeafKey(cameraId, "persistence_windows"),
      defaults.persistenceWindows);
  config.persistenceDwellFraction = configDoubleOr(
      beliefLeafKey(cameraId, "persistence_dwell_fraction"),
      defaults.persistenceDwellFraction);
  config.zoneDwellAlertMs = configInt64Or(
      beliefLeafKey(cameraId, "zone_dwell_alert_ms"),
      defaults.zoneDwellAlertMs);
  config.zoneDwellMonitorMs = configInt64Or(
      beliefLeafKey(cameraId, "zone_dwell_monitor_ms"),
      defaults.zoneDwellMonitorMs);
  config.trackStableAgeMs = configInt64Or(
      beliefLeafKey(cameraId, "track_stable_age_ms"),
      defaults.trackStableAgeMs);
  config.trackJitterDwellMs = configInt64Or(
      beliefLeafKey(cameraId, "track_jitter_dwell_ms"),
      defaults.trackJitterDwellMs);
  config.areaSpreadRatio = configDoubleOr(
      beliefLeafKey(cameraId, "area_spread_ratio"), defaults.areaSpreadRatio);
  config.thresholdCritical = configIntOr(
      beliefLeafKey(cameraId, "threshold_critical"),
      defaults.thresholdCritical);
  config.thresholdHigh = configIntOr(
      beliefLeafKey(cameraId, "threshold_high"), defaults.thresholdHigh);
  config.thresholdMedium = configIntOr(
      beliefLeafKey(cameraId, "threshold_medium"), defaults.thresholdMedium);
  config.thresholdLow = configIntOr(
      beliefLeafKey(cameraId, "threshold_low"), defaults.thresholdLow);
  return config;
}
} // namespace guard_belief
