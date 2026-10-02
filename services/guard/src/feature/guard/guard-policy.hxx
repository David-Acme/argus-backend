#pragma once

#include <camera/identity-state.hxx>
#include <feature/guard/vocabulary/guard-danger.hxx>
#include <shared/vocabulary/guard-mode.hxx>

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>
#include <vector>

struct GuardPersonSignals
{
  int64_t trackId{0};
  int64_t personId{0};
  bool known{false};
  IdentityState identityState{IdentityState::Unrecognized};
  bool identityAvailable{false};
  float identityConfidence{0.0F};
  int identifyAttempts{0};
  double scoreMedian{0.0};
  int scoreSamples{0};
  double area{0.0};
  int zoneWindows{0};
  int trackWindows{0};
  double areaSpread{1.0};
  std::string zoneKind;
  std::string signature;
  std::string observationId;
  int64_t firstSeenMs{0};
  int64_t dwellMs{0};
};

struct GuardContext
{
  GuardMode mode{GuardMode::Home};
  std::string rule;
  std::string severity;
  bool hasKnown{false};
  bool hasUnknown{false};
  int strangerCount{0};
  bool accompaniedByResident{false};
  bool accompaniedByGuest{false};
  bool expectedGuest{false};
  bool inAlertZone{false};
  bool atNight{false};
  bool escalated{false};
  int visitCount{0};
  bool publicPresent{false};
  bool staffOnly{false};
};

struct GuardDeterrenceInput
{
  GuardMode mode{GuardMode::Home};
  GuardDanger danger{GuardDanger::None};
  bool publicPresent{false};
  bool staffOnly{false};
  bool weapon{false};
  bool inAlertZone{false};
  int encounterChecks{0};
};

struct GuardDeterrence
{
  bool voice{false};
  bool alarm{false};
};

struct GuardEventSignals
{
  int64_t cameraId{0};
  std::string cameraName;
  std::string rule;
  std::string severity;
  bool escalated{false};
  bool night{false};
  bool hasKnown{false};
  bool hasUnknown{false};
  int unknownCount{0};
  int knownCount{0};
  int strangerCount{0};
  std::vector<GuardPersonSignals> persons;
  int64_t personId{0};
  int64_t knownPersonId{0};
  int64_t trackId{0};
  int64_t firstSeenMs{0};
  int64_t publishedAtMs{0};
  int64_t dwellMs{0};
  double viewScore{0.0};
  float identityConfidence{0.0F};
  IdentityState identityState{IdentityState::Unrecognized};
  bool identityAvailable{false};
  int identifyAttempts{0};
  double scoreMedian{0.0};
  int scoreSamples{0};
  int zoneWindows{0};
  int trackWindows{0};
  double areaSpread{1.0};
  std::string signature;
  std::string observationId;
  std::string zoneKind;
};

struct GuardEncounterCandidate
{
  int64_t id{0};
  int64_t personId{0};
  std::string signature;
  int64_t lastCameraId{0};
  int64_t lastSeen{0};
};

struct GuardEncounterMatchInput
{
  int64_t personId{0};
  std::string signature;
  int64_t cameraId{0};
  int64_t now{0};
  int64_t windowS{0};
  int64_t continuityWindowS{0};
  double minSimilarity{0.0};
  const std::vector<GuardEncounterCandidate>& candidates;
};

namespace guard_policy
{

GuardDanger evaluate(const GuardContext& context);

GuardDeterrence deterrence(const GuardDeterrenceInput& input);

GuardEventSignals parseObjectEvent(const Json::Value& event);

double signatureSimilarity(const std::string& left, const std::string& right);

std::optional<int64_t> matchEncounter(const GuardEncounterMatchInput& input);

std::string pickGreeting(int64_t seed, const std::vector<std::string>& variants);

GuardMode modeFromString(const std::string& value);
std::string modeToString(GuardMode mode);
std::string dangerToString(GuardDanger danger);
GuardDanger dangerFromString(const std::string& value);
int dangerRank(GuardDanger danger);

inline constexpr double kBaselineHalfLifeS = 7.0 * 24.0 * 3600.0;

double decayBaseline(double stored, int64_t elapsedS);

double baselineNovelty(double decayed);

}
