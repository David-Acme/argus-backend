#pragma once

#include <feature/objects/object-detector.hxx>
#include <feature/operator/known-person-matcher.hxx>
#include <shared/vocabulary/operator-zone.hxx>

#include <camera/event-severity.hxx>

#include <optional>
#include <string>
#include <vector>

struct OperatorState
{
  bool vehiclePreviouslyAbsent{false};
  bool presenceEscalating{false};
};

struct ObjectZoneInput
{
  const DetectedObject& object;
  const OperatorZone& zone;
  int frameWidth{0};
  int frameHeight{0};
};

bool objectCenterInZone(const ObjectZoneInput& input);

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

struct PersonDwellVerdict
{
  int64_t trackId{0};
  bool due{false};
  bool canEmit{false};
};

struct EventIntelligenceInput
{
  int64_t cameraId{0};
  std::vector<DetectedObject> objects;
  std::vector<OperatorZone> zones;
  std::vector<std::string> ignoredClasses;
  bool night{false};
  std::vector<PersonDwellVerdict> persons;
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
  bool night{false};
  std::optional<int64_t> knownPersonId{};
  std::vector<EvaluatedObject> objects{};
};

class EventIntelligence
{
public:
  static EventIntelligenceOutcome evaluate(const EventIntelligenceInput& input);
};
