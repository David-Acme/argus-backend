#pragma once

#include <shared/enums.hxx>

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>
#include <vector>

// Everything the danger matrix needs to know about one camera event.
struct GuardContext
{
  GuardMode mode{GuardMode::Home};
  std::string rule;
  std::string severity;
  bool hasKnown{false};
  bool hasUnknown{false};
  bool trustedCompanion{false};
  bool expectedGuest{false};
  bool inAlertZone{false};
  bool atNight{false};
  bool escalated{false};
  int visitCount{0};
};

// Tri-state face observation carried by one object_detected object.
enum class IdentityState
{
  Known,
  Unrecognized,
  Unobservable,
};

inline std::string identityStateToString(IdentityState state)
{
  switch (state) {
  case IdentityState::Known:
    return "known";
  case IdentityState::Unrecognized:
    return "unrecognized";
  case IdentityState::Unobservable:
    return "unobservable";
  }
  return "unrecognized";
}

// Identity signals derived from one object_detected payload.
struct GuardEventSignals
{
  int64_t cameraId{0};
  std::string cameraName;
  std::string rule;
  std::string severity;
  bool escalated{false};
  bool hasKnown{false};
  bool hasUnknown{false};
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

// Open encounter seen by another camera, used for cross-camera correlation.
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

// Hard floors first: the matrix never lowers a floor, the severity only raises it.
GuardDanger evaluate(const GuardContext& context);

// Binds the event to its root primary track; only that object decides
// known/unknown and the risk signals. Companions are context.
GuardEventSignals parseObjectEvent(const Json::Value& event);

// Histogram-intersection similarity between two encoded appearance signatures.
double signatureSimilarity(const std::string& left, const std::string& right);

// Identity wins, then the closest fresh signature, then same-camera continuity.
std::optional<int64_t> matchEncounter(const GuardEncounterMatchInput& input);

// Rotates the configured greeting variants by encounter; empty input yields "".
std::string pickGreeting(int64_t seed, const std::vector<std::string>& variants);

GuardMode modeFromString(const std::string& value);
std::string modeToString(GuardMode mode);
std::string dangerToString(GuardDanger danger);
GuardDanger dangerFromString(const std::string& value);
int dangerRank(GuardDanger danger);

// Half-life of the per-hour-of-week event rate: one week, so a bucket remembers
// the same hour seven days back at half strength and forgets stale activity.
inline constexpr double kBaselineHalfLifeS = 7.0 * 24.0 * 3600.0;

// Ages a stored event count by the elapsed real time since it was written.
double decayBaseline(double stored, int64_t elapsedS);

// Turns an aged event count into novelty: an empty bucket is fully novel, a
// busy one is routine, and the score never sticks at a single value.
double baselineNovelty(double decayed);

} // namespace guard_policy
