#pragma once

#include <objects/object-detector.hxx>
#include <operator/known-person-matcher.hxx>

#include <shared/enums.hxx>

#include <optional>
#include <string>
#include <vector>

// Normalized (0..1) zone polygon; kind is "alert", "monitor" or "exclude".
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

// Operator evaluation input.
struct EventIntelligenceInput
{
  int64_t cameraId{0};
  std::vector<DetectedObject> objects;
  std::vector<OperatorZone> zones;
  std::vector<std::string> ignoredClasses;
  bool night{false};
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
  std::optional<int64_t> knownPersonId;
};

// Evaluates the 9-rule table in fixed order.
class EventIntelligence
{
public:
  static EventIntelligenceOutcome evaluate(const EventIntelligenceInput& input);
};
