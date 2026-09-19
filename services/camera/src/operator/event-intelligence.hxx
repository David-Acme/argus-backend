#pragma once

#include <objects/object-detector.hxx>
#include <operator/known-person-matcher.hxx>

#include <shared/enums.hxx>

#include <optional>
#include <string>
#include <vector>

// Normalized (0..1) zone polygon; kind is "alert", "monitor" or "exclude".
// The primary track alone decides known/unknown, exclusion and zone; every
// companion travels as context and never votes on the rule.
struct OperatorZone
{
  int64_t cameraId{0};
  std::string name;
  std::string kind;
  std::vector<std::pair<double, double>> points;
};

// Operator rule state across aggregation windows.
struct OperatorState
{
  bool vehiclePreviouslyAbsent{false};
  bool presenceEscalating{false};
};

// Normalized polygon hit-test input.
struct ObjectZoneInput
{
  const DetectedObject& object;
  const OperatorZone& zone;
  int frameWidth{0};
  int frameHeight{0};
};

// True when the object center lies inside the normalized zone polygon.
bool objectCenterInZone(const ObjectZoneInput& input);

// One detection with its identity verdict; personId is 0 when unknown or unmatched.
struct EvaluatedObject
{
  DetectedObject object;
  std::optional<int64_t> personId;
  bool known{false};
  float identityConfidence{0.0F};
  std::string identityState;
  int identifyAttempts{0};
  std::string zoneKind;
};

// Per-track dwell verdict; identity, zone and cooldown stay per person.
struct PersonDwellVerdict
{
  int64_t trackId{0};
  bool due{false};
  bool canEmit{false};
};

// Operator evaluation input.
struct EventIntelligenceInput
{
  int64_t cameraId{0};
  std::vector<DetectedObject> objects;
  std::vector<OperatorZone> zones;
  std::vector<std::string> ignoredClasses;
  bool night{false};
  std::vector<PersonDwellVerdict> persons;
  // Eligible track the rule, identity, crop and zone must all refer to.
  int64_t primaryTrackId{0};
  OperatorState state;
  const IKnownPersonMatcher* matcher{nullptr};
  const uint8_t* frameRgb{nullptr};
  int frameWidth{0};
  int frameHeight{0};
};

struct EventIntelligenceOutcome
{
  bool publish{false};
  std::string rule;
  EventSeverity severity{EventSeverity::Info};
  bool escalated{false};
  std::optional<int64_t> knownPersonId{};
  std::vector<EvaluatedObject> objects{};
};

// Evaluates the 9-rule table in fixed order.
class EventIntelligence
{
public:
  static EventIntelligenceOutcome evaluate(const EventIntelligenceInput& input);
};
