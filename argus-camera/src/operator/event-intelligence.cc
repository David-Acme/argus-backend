#include <operator/event-intelligence.hxx>

#include <algorithm>
#include <string>

namespace
{
bool centerInZone(const DetectedObject& object, const OperatorZone& zone,
                  int frameWidth, int frameHeight)
{
  if (zone.points.size() < 3)
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

bool objectInZoneKind(const DetectedObject& object,
                      const std::vector<OperatorZone>& zones,
                      const std::string& kind, int frameWidth, int frameHeight)
{
  for (const auto& zone : zones) {
    if (zone.kind == kind &&
        centerInZone(object, zone, frameWidth, frameHeight))
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

  for (const auto& object : input.objects) {
    if (objectInZoneKind(object, input.zones, "exclude", input.frameWidth,
                         input.frameHeight)) {
      EventIntelligenceOutcome dropped;
      dropped.publish = false;
      dropped.rule = "exclude_zone";
      return dropped;
    }
  }

  const bool hasPerson = containsClass(input.objects, "person");
  const bool hasVehicle = containsClass(input.objects, "car") ||
                          containsClass(input.objects, "truck") ||
                          containsClass(input.objects, "bus") ||
                          containsClass(input.objects, "motorcycle");

  if (input.matcher && hasPerson && input.frameRgb) {
    for (const auto& object : input.objects) {
      if (object.name != "person")
        continue;
      const PersonCrop crop{.rgb = input.frameRgb,
                            .width = input.frameWidth,
                            .height = input.frameHeight,
                            .x = object.x,
                            .y = object.y,
                            .w = object.w,
                            .h = object.h};
      if (const auto personId = input.matcher->match(crop)) {
        EventIntelligenceOutcome known;
        known.publish = true;
        known.rule = "known_person";
        known.severity = EventSeverity::Info;
        known.knownPersonId = personId;
        return known;
      }
    }
  }

  if (hasPerson) {
    for (const auto& object : input.objects) {
      if (object.name == "person" &&
          objectInZoneKind(object, input.zones, "alert", input.frameWidth,
                           input.frameHeight)) {
        EventIntelligenceOutcome alert;
        alert.publish = true;
        alert.rule = "person_in_alert_zone";
        alert.severity = EventSeverity::Critical;
        return alert;
      }
    }
  }

  if (hasPerson) {
    for (const auto& object : input.objects) {
      if (object.name == "person" &&
          objectInZoneKind(object, input.zones, "monitor", input.frameWidth,
                           input.frameHeight)) {
        EventIntelligenceOutcome monitor;
        monitor.publish = true;
        monitor.rule = "person_in_monitor_zone";
        monitor.severity = EventSeverity::Warning;
        return monitor;
      }
    }
  }

  if (hasPerson) {
    EventIntelligenceOutcome person;
    person.publish = true;
    person.rule = input.night ? "person_night" : "person_day";
    person.severity = input.night ? EventSeverity::Warning : EventSeverity::Info;
    return person;
  }

  if (hasVehicle && input.state.vehiclePreviouslyAbsent && !input.night) {
    EventIntelligenceOutcome arrival;
    arrival.publish = true;
    arrival.rule = "vehicle_arrival";
    arrival.severity = EventSeverity::Info;
    return arrival;
  }

  if (hasVehicle && (input.night || input.state.presenceEscalating)) {
    EventIntelligenceOutcome vehicle;
    vehicle.publish = true;
    vehicle.rule = input.night ? "vehicle_night" : "presence_escalating";
    vehicle.severity = EventSeverity::Warning;
    vehicle.escalated = input.state.presenceEscalating;
    return vehicle;
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
