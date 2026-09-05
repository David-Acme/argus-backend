#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <operator/event-intelligence.hxx>
#include <operator/known-person-matcher.hxx>

#include <cstdint>
#include <optional>
#include <vector>

namespace
{
DetectedObject personAt(float x, float y, float w, float h)
{
  DetectedObject object;
  object.name = "person";
  object.cls = 0;
  object.confidence = 0.9f;
  object.x = x;
  object.y = y;
  object.w = w;
  object.h = h;
  return object;
}

DetectedObject vehicleAt(float x, float y)
{
  DetectedObject object;
  object.name = "car";
  object.cls = 2;
  object.confidence = 0.8f;
  object.x = x;
  object.y = y;
  object.w = 200;
  object.h = 120;
  return object;
}

// Full-frame square polygon in normalized coordinates.
OperatorZone squareZone(const char* kind, float fromX, float fromY, float toX,
                        float toY)
{
  OperatorZone zone;
  zone.cameraId = 1;
  zone.name = kind;
  zone.kind = kind;
  zone.points = {{fromX, fromY}, {toX, fromY}, {toX, toY}, {fromX, toY}};
  return zone;
}

EventIntelligenceInput baseInput(std::vector<DetectedObject> objects)
{
  EventIntelligenceInput input;
  input.cameraId = 1;
  input.objects = std::move(objects);
  input.frameWidth = 640;
  input.frameHeight = 480;
  return input;
}

// A stub matcher that matches every person crop as person 7.
class KnownPerson7Matcher final : public IKnownPersonMatcher
{
public:
  std::optional<int64_t> match(const PersonCrop&) const override
  {
    return int64_t{7};
  }
};
} // namespace

TEST_CASE("rule 1: an object centered in an exclude zone drops the event")
{
  auto input = baseInput({personAt(280, 200, 80, 160)});
  input.zones.push_back(squareZone("exclude", 0.25f, 0.25f, 0.75f, 0.75f));

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK_FALSE(outcome.publish);
  CHECK(outcome.rule == "exclude_zone");
}

TEST_CASE("rule 3: person inside an alert zone is critical")
{
  auto input = baseInput({personAt(280, 200, 80, 160)});
  input.zones.push_back(squareZone("alert", 0.25f, 0.25f, 0.75f, 0.75f));

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "person_in_alert_zone");
  CHECK(outcome.severity == EventSeverity::Critical);
}

TEST_CASE("rule 4: person inside a monitor zone is warning")
{
  auto input = baseInput({personAt(280, 200, 80, 160)});
  input.zones.push_back(squareZone("monitor", 0.25f, 0.25f, 0.75f, 0.75f));

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "person_in_monitor_zone");
  CHECK(outcome.severity == EventSeverity::Warning);
}

TEST_CASE("rules 5 and 6: person by the night schedule")
{
  auto night = baseInput({personAt(0, 0, 80, 160)});
  night.night = true;
  const auto nightOutcome = EventIntelligence::evaluate(night);
  CHECK(nightOutcome.publish);
  CHECK(nightOutcome.rule == "person_night");
  CHECK(nightOutcome.severity == EventSeverity::Warning);

  const auto dayOutcome = EventIntelligence::evaluate(baseInput({personAt(0, 0, 80, 160)}));
  CHECK(dayOutcome.publish);
  CHECK(dayOutcome.rule == "person_day");
  CHECK(dayOutcome.severity == EventSeverity::Info);
}

TEST_CASE("rule 7: vehicle arrival after an absence is info")
{
  auto input = baseInput({vehicleAt(10, 10)});
  input.state.vehiclePreviouslyAbsent = true;

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "vehicle_arrival");
  CHECK(outcome.severity == EventSeverity::Info);
}

TEST_CASE("rule 8: vehicle at night and escalating presence are warning")
{
  auto night = baseInput({vehicleAt(10, 10)});
  night.night = true;
  const auto nightOutcome = EventIntelligence::evaluate(night);
  CHECK(nightOutcome.publish);
  CHECK(nightOutcome.rule == "vehicle_night");
  CHECK(nightOutcome.severity == EventSeverity::Warning);

  auto escalating = baseInput({vehicleAt(10, 10)});
  escalating.state.presenceEscalating = true;
  const auto escalatingOutcome = EventIntelligence::evaluate(escalating);
  CHECK(escalatingOutcome.publish);
  CHECK(escalatingOutcome.rule == "presence_escalating");
  CHECK(escalatingOutcome.severity == EventSeverity::Warning);
  CHECK(escalatingOutcome.escalated);
}

TEST_CASE("rule 9: an ignored class drops the event")
{
  DetectedObject cat;
  cat.name = "cat";
  cat.cls = 15;
  cat.confidence = 0.7f;

  auto input = baseInput({cat});
  input.ignoredClasses = {"cat"};

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK_FALSE(outcome.publish);
  CHECK(outcome.rule == "ignored_class");
}

TEST_CASE("no matching rule drops the event")
{
  DetectedObject dog;
  dog.name = "dog";
  dog.cls = 16;
  dog.confidence = 0.7f;

  const auto outcome = EventIntelligence::evaluate(baseInput({dog}));
  CHECK_FALSE(outcome.publish);
  CHECK(outcome.rule == "no_rule");
}

TEST_CASE("empty detections drop the event")
{
  const auto outcome = EventIntelligence::evaluate(baseInput({}));
  CHECK_FALSE(outcome.publish);
  CHECK(outcome.rule.empty());
}

TEST_CASE("rule 2: a matched known person dominates the zone rules")
{
  KnownPerson7Matcher matcher;
  auto input = baseInput({personAt(280, 200, 80, 160)});
  input.zones.push_back(squareZone("alert", 0.25f, 0.25f, 0.75f, 0.75f));
  input.matcher = &matcher;
  static std::vector<uint8_t> frame(640 * 480 * 3, 128);
  input.frameRgb = frame.data();

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "known_person");
  CHECK(outcome.severity == EventSeverity::Info);
  CHECK(outcome.knownPersonId.has_value());
  CHECK(*outcome.knownPersonId == 7);
}

TEST_CASE("the no-match matcher keeps the zone rule severity")
{
  NoKnownPersonMatcher matcher;
  auto input = baseInput({personAt(280, 200, 80, 160)});
  input.zones.push_back(squareZone("alert", 0.25f, 0.25f, 0.75f, 0.75f));
  input.matcher = &matcher;
  static std::vector<uint8_t> frame(640 * 480 * 3, 128);
  input.frameRgb = frame.data();

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "person_in_alert_zone");
  CHECK(outcome.severity == EventSeverity::Critical);
  CHECK_FALSE(outcome.knownPersonId.has_value());
}

TEST_CASE("a vehicle arriving before a person keeps the person rule dominant")
{
  auto input = baseInput({vehicleAt(10, 10), personAt(0, 0, 80, 160)});
  input.state.vehiclePreviouslyAbsent = true;

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "person_day");
}