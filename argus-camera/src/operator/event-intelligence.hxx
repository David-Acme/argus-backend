#pragma once

#include <objects/object-detector.hxx>
#include <operator/known-person-matcher.hxx>

#include <shared/enums.hxx>

#include <optional>
#include <string>
#include <vector>

// Zone polygons are normalized (0..1) frame coordinates, scoped to one camera.
struct OperatorZone
{
  int64_t cameraId{0};
  std::string name;
  // "alert", "monitor" or "exclude".
  std::string kind;
  std::vector<std::pair<double, double>> points;
};

struct OperatorState
{
  // Rule 7: the vehicle class reappeared after at least an arrival gap.
  bool vehiclePreviouslyAbsent{false};
  // Rule 8: presence kept growing over consecutive aggregation windows.
  bool presenceEscalating{false};
};

struct EventIntelligenceInput
{
  int64_t cameraId{0};
  std::vector<DetectedObject> objects;
  std::vector<OperatorZone> zones;
  std::vector<std::string> ignoredClasses;
  bool night{false};
  OperatorState state;
  // Absent (or configured as no-match) keeps rules 3-8 severities; present
  // and matching makes rule 2 known_person dominate (Ruling AC).
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

// The 9-rule table of Appendix B.3, evaluated in fixed order:
// 1 exclude_zone drop, 2 known_person (dominates 3-8 when matched),
// 3 person_in_alert_zone Critical, 4 person_in_monitor_zone Warning,
// 5 person_night Warning, 6 person_day Info, 7 vehicle_arrival Info,
// 8 vehicle_night / escalating presence Warning, 9 ignored_class drop.
class EventIntelligence
{
public:
  static EventIntelligenceOutcome evaluate(const EventIntelligenceInput& input);
};
