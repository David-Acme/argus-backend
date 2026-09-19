#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <guard-belief.hxx>
#include <guard-policy.hxx>
#include <shared/services/config-service/config-service.hxx>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "temp-db.hxx"

using guard_test::tempCounter;
using guard_test::tempPath;

namespace
{
BeliefInput intruderInput()
{
  BeliefInput input;
  input.scoreMedian = 0.9;
  input.scoreSamples = 4;
  input.dwellMs = 5000;
  input.zoneWindows = 3;
  input.trackWindows = 4;
  input.zoneKind = "alert";
  input.trackAgeMs = 6000;
  input.areaSpread = 1.2;
  input.identity = IdentityState::Unrecognized;
  input.healthDegraded = false;
  return input;
}

bool hasSignal(const BeliefResult& result, BeliefSignal signal)
{
  for (const auto& applied : result.signals) {
    if (applied == signal)
      return true;
  }
  return false;
}

} // namespace

TEST_CASE("each detector signal contributes its bounded weight")
{
  BeliefInput strong = intruderInput();
  strong.scoreMedian = 0.9;
  strong.scoreSamples = 4;
  const auto strongResult = guard_belief::evaluateBelief(strong);
  CHECK(hasSignal(strongResult, BeliefSignal::DetectorStrong));
  CHECK_FALSE(hasSignal(strongResult, BeliefSignal::DetectorWeak));

  BeliefInput weak = intruderInput();
  weak.scoreMedian = 0.2;
  weak.scoreSamples = 3;
  const auto weakResult = guard_belief::evaluateBelief(weak);
  CHECK(hasSignal(weakResult, BeliefSignal::DetectorWeak));
  CHECK_FALSE(hasSignal(weakResult, BeliefSignal::DetectorStrong));

  BeliefInput mid = intruderInput();
  mid.scoreMedian = 0.5;
  mid.scoreSamples = 3;
  const auto midResult = guard_belief::evaluateBelief(mid);
  CHECK_FALSE(hasSignal(midResult, BeliefSignal::DetectorStrong));
  CHECK_FALSE(hasSignal(midResult, BeliefSignal::DetectorWeak));

  BeliefInput none = intruderInput();
  none.scoreMedian = 0.9;
  none.scoreSamples = 0;
  const auto noneResult = guard_belief::evaluateBelief(none);
  CHECK_FALSE(hasSignal(noneResult, BeliefSignal::DetectorStrong));
  CHECK_FALSE(hasSignal(noneResult, BeliefSignal::DetectorWeak));
}

TEST_CASE("persistence needs both zone windows and zone dwell")
{
  const auto met = guard_belief::evaluateBelief(intruderInput());
  CHECK(hasSignal(met, BeliefSignal::PersistenceMet));

  BeliefInput fewWindows = intruderInput();
  fewWindows.zoneWindows = 1;
  const auto few = guard_belief::evaluateBelief(fewWindows);
  CHECK_FALSE(hasSignal(few, BeliefSignal::PersistenceMet));
  CHECK_FALSE(hasSignal(few, BeliefSignal::PersistenceShort));

  BeliefInput shortDwell = intruderInput();
  shortDwell.dwellMs = 800;
  const auto shortResult = guard_belief::evaluateBelief(shortDwell);
  CHECK(hasSignal(shortResult, BeliefSignal::PersistenceShort));
  CHECK_FALSE(hasSignal(shortResult, BeliefSignal::PersistenceMet));
}

TEST_CASE("track stability and jitter read different track facts")
{
  const auto stable = guard_belief::evaluateBelief(intruderInput());
  CHECK(hasSignal(stable, BeliefSignal::TrackStable));
  CHECK_FALSE(hasSignal(stable, BeliefSignal::TrackJitter));

  BeliefInput fresh = intruderInput();
  fresh.trackWindows = 1;
  fresh.trackAgeMs = 400;
  const auto jittered = guard_belief::evaluateBelief(fresh);
  CHECK(hasSignal(jittered, BeliefSignal::TrackJitter));
  CHECK_FALSE(hasSignal(jittered, BeliefSignal::TrackStable));

  BeliefInput shaky = intruderInput();
  shaky.areaSpread = 3.5;
  const auto shakyResult = guard_belief::evaluateBelief(shaky);
  CHECK(hasSignal(shakyResult, BeliefSignal::TrackStable));
  CHECK(hasSignal(shakyResult, BeliefSignal::TrackJitter));
}

TEST_CASE("identity states move belief in opposite directions")
{
  BeliefInput known = intruderInput();
  known.identity = IdentityState::Known;
  const auto knownResult = guard_belief::evaluateBelief(known);
  CHECK(hasSignal(knownResult, BeliefSignal::IdentityKnown));
  CHECK(knownResult.score == 2);

  BeliefInput unobservable = intruderInput();
  unobservable.identity = IdentityState::Unobservable;
  const auto unobservableResult = guard_belief::evaluateBelief(unobservable);
  CHECK(hasSignal(unobservableResult, BeliefSignal::IdentityUnobservable));
  CHECK(unobservableResult.score == 3);

  const auto unrecognized = guard_belief::evaluateBelief(intruderInput());
  CHECK(hasSignal(unrecognized, BeliefSignal::IdentityUnrecognized));
  CHECK(unrecognized.score == 7);
}

TEST_CASE("unavailable identity scores zero instead of unrecognized")
{
  BeliefInput input = intruderInput();
  input.identityAvailable = false;
  const auto result = guard_belief::evaluateBelief(input);
  CHECK(hasSignal(result, BeliefSignal::IdentityUnavailable));
  CHECK_FALSE(hasSignal(result, BeliefSignal::IdentityUnrecognized));
  CHECK(result.score == 5);
}

TEST_CASE("degraded camera health withholds belief")
{
  BeliefInput degraded = intruderInput();
  degraded.healthDegraded = true;
  const auto result = guard_belief::evaluateBelief(degraded);
  CHECK(hasSignal(result, BeliefSignal::CameraHealthDegraded));
  CHECK(result.score == 5);
}

TEST_CASE("thresholds are asymmetric and decreasing in severity")
{
  const BeliefConfig config;
  CHECK(guard_belief::beliefThreshold(GuardDanger::Critical, config) == 1);
  CHECK(guard_belief::beliefThreshold(GuardDanger::High, config) == 3);
  CHECK(guard_belief::beliefThreshold(GuardDanger::Medium, config) == 5);
  CHECK(guard_belief::beliefThreshold(GuardDanger::Low, config) == 7);
  CHECK(guard_belief::beliefThreshold(GuardDanger::Critical, config) <
        guard_belief::beliefThreshold(GuardDanger::High, config));
  CHECK(guard_belief::beliefThreshold(GuardDanger::High, config) <
        guard_belief::beliefThreshold(GuardDanger::Medium, config));
}

TEST_CASE("the motivating regression suppresses in enforce but notifies in legacy")
{
  BeliefInput regression;
  regression.scoreMedian = 0.3;
  regression.scoreSamples = 3;
  regression.dwellMs = 800;
  regression.zoneWindows = 1;
  regression.trackWindows = 1;
  regression.zoneKind = "alert";
  regression.trackAgeMs = 800;
  regression.areaSpread = 1.0;
  regression.identity = IdentityState::Unobservable;
  regression.healthDegraded = false;

  const auto result = guard_belief::evaluateBelief(regression);
  CHECK(hasSignal(result, BeliefSignal::DetectorWeak));
  CHECK(hasSignal(result, BeliefSignal::PersistenceShort));
  CHECK(hasSignal(result, BeliefSignal::TrackJitter));
  CHECK(hasSignal(result, BeliefSignal::IdentityUnobservable));
  CHECK(result.score == -8);

  constexpr GuardDanger severity = GuardDanger::Critical;
  CHECK(guard_policy::dangerRank(severity) >= 2);
  CHECK(result.score <
        guard_belief::beliefThreshold(severity, regression.config));
}

TEST_CASE("out-of-range observations clamp instead of breaking the score")
{
  BeliefInput input = intruderInput();
  input.scoreMedian = 4.5;
  input.scoreSamples = -2;
  input.dwellMs = -100;
  input.zoneWindows = -1;
  input.trackWindows = -1;
  input.trackAgeMs = -50;
  input.areaSpread = 0.0;
  const auto result = guard_belief::evaluateBelief(input);
  CHECK(hasSignal(result, BeliefSignal::IdentityUnrecognized));
  CHECK(result.score == -2);
}

TEST_CASE("custom weights reshape the score")
{
  BeliefInput input = intruderInput();
  input.config.weightDetectorStrong = 5;
  const auto result = guard_belief::evaluateBelief(input);
  CHECK(result.score == 10);
}

TEST_CASE("unresolved config matches the member defaults exactly")
{
  const BeliefConfig resolved = guard_belief::resolveBeliefConfig(99);
  const BeliefConfig defaults;
  CHECK(resolved.weightDetectorStrong == defaults.weightDetectorStrong);
  CHECK(resolved.weightDetectorWeak == defaults.weightDetectorWeak);
  CHECK(resolved.weightPersistenceMet == defaults.weightPersistenceMet);
  CHECK(resolved.weightPersistenceShort == defaults.weightPersistenceShort);
  CHECK(resolved.weightTrackStable == defaults.weightTrackStable);
  CHECK(resolved.weightTrackJitter == defaults.weightTrackJitter);
  CHECK(resolved.weightIdentityUnrecognized ==
        defaults.weightIdentityUnrecognized);
  CHECK(resolved.weightIdentityUnobservable ==
        defaults.weightIdentityUnobservable);
  CHECK(resolved.weightIdentityUnavailable ==
        defaults.weightIdentityUnavailable);
  CHECK(resolved.weightIdentityKnown == defaults.weightIdentityKnown);
  CHECK(resolved.weightCameraHealthDegraded ==
        defaults.weightCameraHealthDegraded);
  CHECK(resolved.detectorStrong == doctest::Approx(defaults.detectorStrong));
  CHECK(resolved.detectorWeak == doctest::Approx(defaults.detectorWeak));
  CHECK(resolved.minScoreSamples == defaults.minScoreSamples);
  CHECK(resolved.persistenceWindows == defaults.persistenceWindows);
  CHECK(resolved.persistenceDwellFraction ==
        doctest::Approx(defaults.persistenceDwellFraction));
  CHECK(resolved.zoneDwellAlertMs == defaults.zoneDwellAlertMs);
  CHECK(resolved.zoneDwellMonitorMs == defaults.zoneDwellMonitorMs);
  CHECK(resolved.trackStableAgeMs == defaults.trackStableAgeMs);
  CHECK(resolved.trackJitterDwellMs == defaults.trackJitterDwellMs);
  CHECK(resolved.areaSpreadRatio == doctest::Approx(defaults.areaSpreadRatio));
  CHECK(resolved.thresholdCritical == defaults.thresholdCritical);
  CHECK(resolved.thresholdHigh == defaults.thresholdHigh);
  CHECK(resolved.thresholdMedium == defaults.thresholdMedium);
  CHECK(resolved.thresholdLow == defaults.thresholdLow);
}

TEST_CASE("per-camera overrides win over the global defaults")
{
  ConfigService::setRuntimeString("guard.belief.camera.7.threshold_medium",
                                  "6");
  const BeliefConfig overridden = guard_belief::resolveBeliefConfig(7);
  CHECK(overridden.thresholdMedium == 6);
  const BeliefConfig plain = guard_belief::resolveBeliefConfig(8);
  CHECK(plain.thresholdMedium == 5);
  CHECK(plain.thresholdCritical == 1);

  const std::string path = tempPath("guard-belief-test-" +
                                    std::to_string(::getpid()) + "-" +
                                    std::to_string(tempCounter()) + ".toml");
  {
    std::ofstream out(path, std::ios::trunc);
    out << "[guard.belief]\nthreshold_medium = 9\n"
           "[guard.belief.camera.\"7\"]\nthreshold_high = 4\n";
  }
  ConfigService::load(path);
  std::remove(path.c_str());
  const BeliefConfig fromFile = guard_belief::resolveBeliefConfig(7);
  CHECK(fromFile.thresholdHigh == 4);
  const BeliefConfig other = guard_belief::resolveBeliefConfig(8);
  CHECK(other.thresholdMedium == 9);
}

TEST_CASE("gate scope parsing round-trips and rejects unknown values")
{
  CHECK(beliefGateScopeFromString("notify") == BeliefGateScope::Notify);
  CHECK(beliefGateScopeFromString("communication") ==
        BeliefGateScope::Communication);
  CHECK(beliefGateScopeFromString("all") == BeliefGateScope::All);
  CHECK_FALSE(beliefGateScopeFromString("bogus").has_value());
  CHECK_FALSE(beliefGateScopeFromString("").has_value());
  CHECK(beliefGateScopeToString(BeliefGateScope::Notify) == "notify");
  CHECK(beliefGateScopeToString(BeliefGateScope::Communication) ==
        "communication");
  CHECK(beliefGateScopeToString(BeliefGateScope::All) == "all");
}

TEST_CASE("gate scope covers notify always, physical only without hard floor")
{
  struct ScopeRow
  {
    BeliefGateScope scope{BeliefGateScope::Notify};
    GuardActionKind kind{GuardActionKind::Notify};
    bool hardFloor{false};
    bool covered{false};
  };
  const std::vector<ScopeRow> matrix = {
      {.scope = BeliefGateScope::Notify,
       .kind = GuardActionKind::Notify,
       .hardFloor = false,
       .covered = true},
      {.scope = BeliefGateScope::Notify,
       .kind = GuardActionKind::Notify,
       .hardFloor = true,
       .covered = true},
      {.scope = BeliefGateScope::Communication,
       .kind = GuardActionKind::Notify,
       .hardFloor = true,
       .covered = true},
      {.scope = BeliefGateScope::All,
       .kind = GuardActionKind::Notify,
       .hardFloor = true,
       .covered = true},
      {.scope = BeliefGateScope::Notify,
       .kind = GuardActionKind::Announce,
       .hardFloor = false,
       .covered = false},
      {.scope = BeliefGateScope::Communication,
       .kind = GuardActionKind::Announce,
       .hardFloor = true,
       .covered = true},
      {.scope = BeliefGateScope::All,
       .kind = GuardActionKind::Announce,
       .hardFloor = false,
       .covered = true},
      {.scope = BeliefGateScope::Notify,
       .kind = GuardActionKind::Alarm,
       .hardFloor = false,
       .covered = false},
      {.scope = BeliefGateScope::Communication,
       .kind = GuardActionKind::Alarm,
       .hardFloor = false,
       .covered = false},
      {.scope = BeliefGateScope::All,
       .kind = GuardActionKind::Alarm,
       .hardFloor = false,
       .covered = true},
      {.scope = BeliefGateScope::All,
       .kind = GuardActionKind::Alarm,
       .hardFloor = true,
       .covered = false},
      {.scope = BeliefGateScope::All,
       .kind = GuardActionKind::SirenArm,
       .hardFloor = false,
       .covered = true},
      {.scope = BeliefGateScope::All,
       .kind = GuardActionKind::SirenArm,
       .hardFloor = true,
       .covered = false},
      {.scope = BeliefGateScope::All,
       .kind = GuardActionKind::Greet,
       .hardFloor = false,
       .covered = false},
      {.scope = BeliefGateScope::All,
       .kind = GuardActionKind::Listen,
       .hardFloor = false,
       .covered = false},
      {.scope = BeliefGateScope::All,
       .kind = GuardActionKind::SirenDisarm,
       .hardFloor = false,
       .covered = false}};
  for (const auto& row : matrix)
    CHECK(guard_belief::beliefSuppressesKind(
              {.scope = row.scope, .kind = row.kind, .hardFloor = row.hardFloor}) ==
          row.covered);
}
