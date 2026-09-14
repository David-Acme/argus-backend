#include <operator/event-intelligence.hxx>

#include <algorithm>
#include <string>

bool objectCenterInZone(const ObjectZoneInput& input)
{
  const DetectedObject& object = input.object;
  const OperatorZone& zone = input.zone;
  const int frameWidth = input.frameWidth;
  const int frameHeight = input.frameHeight;
  if (zone.points.size() < 3 || frameWidth <= 0 || frameHeight <= 0)
    return false;

  const double cx = (object.x + object.w / 2) / frameWidth;
  const double cy = (object.y + object.h / 2) / frameHeight;

  bool inside = false;
  for (size_t i = 0, j = zone.points.size() - 1; i < zone.points.size();
       j = i++) {
    const double xi = zone.points[i].first;
    const double yi = zone.points[i].second;
    const double xj = zone.points[j].first;
    const double yj = zone.points[j].second;
    const bool crosses =
        (yi > cy) != (yj > cy) && cx < (xj - xi) * (cy - yi) / (yj - yi + 1e-12) + xi;
    if (crosses)
      inside = !inside;
  }
  return inside;
}

namespace
{
bool centerInZone(const ObjectZoneInput& input)
{
  return objectCenterInZone(input);
}

struct ObjectInZoneKindInput
{
  const DetectedObject& object;
  const std::vector<OperatorZone>& zones;
  const std::string& kind;
  int frameWidth{0};
  int frameHeight{0};
};

bool objectInZoneKind(const ObjectInZoneKindInput& input)
{
  const DetectedObject& object = input.object;
  const int frameWidth = input.frameWidth;
  const int frameHeight = input.frameHeight;
  for (const auto& zone : input.zones) {
    if (zone.kind == input.kind &&
        centerInZone({.object = object, .zone = zone,
                      .frameWidth = frameWidth, .frameHeight = frameHeight}))
      return true;
  }
  return false;
}

bool containsClass(const std::vector<DetectedObject>& objects,
                   const std::string& name)
{
  return std::any_of(objects.begin(), objects.end(),
                     [&](const DetectedObject& object) {
                       return object.name == name;
                     });
}

bool isIgnored(const std::vector<DetectedObject>& objects,
               const std::vector<std::string>& ignoredClasses)
{
  return std::all_of(objects.begin(), objects.end(),
                     [&](const DetectedObject& object) {
                       return std::find(ignoredClasses.begin(),
                                        ignoredClasses.end(),
                                        object.name) != ignoredClasses.end();
                     });
}
} // namespace

EventIntelligenceOutcome
EventIntelligence::evaluate(const EventIntelligenceInput& input)
{
  if (input.objects.empty())
    return {};

  const auto isExcluded = [&input](const DetectedObject& object) {
    return objectInZoneKind({.object = object,
                             .zones = input.zones,
                             .kind = "exclude",
                             .frameWidth = input.frameWidth,
                             .frameHeight = input.frameHeight});
  };

  if (input.primaryTrackId != 0) {
    const auto primaryObject = std::find_if(
        input.objects.begin(), input.objects.end(),
        [&input](const DetectedObject& object) {
          return object.trackId == input.primaryTrackId;
        });
    if (primaryObject != input.objects.end() && isExcluded(*primaryObject)) {
      EventIntelligenceOutcome dropped;
      dropped.publish = false;
      dropped.rule = "exclude_zone";
      return dropped;
    }
  }
  else {
    for (const auto& object : input.objects) {
      if (isExcluded(object)) {
        EventIntelligenceOutcome dropped;
        dropped.publish = false;
        dropped.rule = "exclude_zone";
        return dropped;
      }
    }
  }

  const bool hasPerson = containsClass(input.objects, "person");
  const auto verdictFor =
      [&input](int64_t trackId) -> const PersonDwellVerdict* {
    const auto found = std::find_if(
        input.persons.begin(), input.persons.end(),
        [trackId](const PersonDwellVerdict& verdict) {
          return verdict.trackId == trackId;
        });
    return found == input.persons.end() ? nullptr : &*found;
  };
  const auto dueFor = [&verdictFor](int64_t trackId) {
    const PersonDwellVerdict* verdict = verdictFor(trackId);
    return verdict != nullptr && verdict->due;
  };
  const auto isPrimary = [&input](int64_t trackId) {
    return input.primaryTrackId == 0 || trackId == input.primaryTrackId;
  };
  const bool personDue =
      hasPerson &&
      (input.primaryTrackId != 0
           ? dueFor(input.primaryTrackId)
           : std::any_of(input.persons.begin(), input.persons.end(),
                         [](const PersonDwellVerdict& verdict) {
                           return verdict.due;
                         }));
  const bool hasVehicle = containsClass(input.objects, "car") ||
                          containsClass(input.objects, "truck") ||
                          containsClass(input.objects, "bus") ||
                          containsClass(input.objects, "motorcycle");

  std::vector<EvaluatedObject> evaluated;
  evaluated.reserve(input.objects.size());
  for (const auto& object : input.objects)
    evaluated.push_back({.object = object,
                         .personId = std::nullopt,
                         .known = false,
                         .identityConfidence = 0.0F,
                         .zoneKind = {}});

  std::optional<int64_t> knownPersonId;
  if (input.matcher && personDue && input.frameRgb) {
    for (auto& entry : evaluated) {
      if (entry.object.name != "person" || !dueFor(entry.object.trackId))
        continue;
      const PersonCrop crop{.cameraId = input.cameraId,
                            .trackId = entry.object.trackId,
                            .firstSeenMs = entry.object.firstSeenMs,
                            .rgb = input.frameRgb,
                            .width = input.frameWidth,
                            .height = input.frameHeight,
                            .x = entry.object.x,
                            .y = entry.object.y,
                            .w = entry.object.w,
                            .h = entry.object.h};
      const auto match = input.matcher->match(crop);
      if (!match)
        continue;
      entry.personId = match->personId;
      entry.known = match->identity == PersonIdentity::Known;
      entry.identityConfidence = match->confidence;
      if (entry.known && isPrimary(entry.object.trackId) && !knownPersonId)
        knownPersonId = match->personId;
    }
  }

  for (auto& entry : evaluated) {
    if (entry.object.name != "person")
      continue;
    if (objectInZoneKind({.object = entry.object,
                          .zones = input.zones,
                          .kind = "alert",
                          .frameWidth = input.frameWidth,
                          .frameHeight = input.frameHeight}))
      entry.zoneKind = "alert";
    else if (objectInZoneKind({.object = entry.object,
                               .zones = input.zones,
                               .kind = "monitor",
                               .frameWidth = input.frameWidth,
                               .frameHeight = input.frameHeight}))
      entry.zoneKind = "monitor";
  }

  const auto finalize = [&](EventIntelligenceOutcome outcome) {
    outcome.knownPersonId = knownPersonId;
    outcome.objects = evaluated;
    return outcome;
  };

  const EvaluatedObject* primary = nullptr;
  if (input.primaryTrackId != 0) {
    const auto found = std::find_if(
        evaluated.begin(), evaluated.end(), [&input](const EvaluatedObject& e) {
          return e.object.name == "person" &&
                 e.object.trackId == input.primaryTrackId;
        });
    if (found != evaluated.end())
      primary = &*found;
  }
  else {
    double bestArea = -1.0;
    for (const auto& entry : evaluated) {
      if (entry.object.name != "person")
        continue;
      const double area = entry.object.w * entry.object.h;
      if (area >= bestArea) {
        bestArea = area;
        primary = &entry;
      }
    }
  }
  if (primary != nullptr && primary->known && knownPersonId)
    return finalize({.publish = true,
                     .rule = "known_person",
                     .severity = EventSeverity::Info});

  if (personDue) {
    for (const auto& entry : evaluated) {
      if (entry.object.name == "person" && isPrimary(entry.object.trackId) &&
          dueFor(entry.object.trackId) && entry.zoneKind == "alert") {
        return finalize({.publish = true,
                         .rule = "person_in_alert_zone",
                         .severity = EventSeverity::Critical});
      }
    }
  }

  if (personDue) {
    for (const auto& entry : evaluated) {
      if (entry.object.name == "person" && isPrimary(entry.object.trackId) &&
          dueFor(entry.object.trackId) && entry.zoneKind == "monitor") {
        return finalize({.publish = true,
                         .rule = "person_in_monitor_zone",
                         .severity = EventSeverity::Warning});
      }
    }
  }

  if (personDue) {
    return finalize(
        {.publish = true,
         .rule = input.night ? "person_night" : "person_day",
         .severity = input.night ? EventSeverity::Warning : EventSeverity::Info});
  }

  if (hasVehicle && input.state.vehiclePreviouslyAbsent && !input.night) {
    return finalize({.publish = true,
                     .rule = "vehicle_arrival",
                     .severity = EventSeverity::Info});
  }

  if (hasVehicle && (input.night || input.state.presenceEscalating)) {
    return finalize({.publish = true,
                     .rule = input.night ? "vehicle_night"
                                         : "presence_escalating",
                     .severity = EventSeverity::Warning,
                     .escalated = input.state.presenceEscalating});
  }

  if (isIgnored(input.objects, input.ignoredClasses)) {
    EventIntelligenceOutcome ignored;
    ignored.publish = false;
    ignored.rule = "ignored_class";
    return ignored;
  }

  EventIntelligenceOutcome dropped;
  dropped.publish = false;
  dropped.rule = "no_rule";
  return dropped;
}
