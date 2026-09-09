#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <operator/event-intelligence.hxx>
#include <operator/known-person-matcher.hxx>

#include <cstdint>
#include <optional>
#include <vector>

namespace
{
struct PersonAtInput
{
  float x{0};
  float y{0};
  float w{0};
  float h{0};
};

DetectedObject personAt(const PersonAtInput& input)
{
  DetectedObject object;
  object.x = input.x;
  object.y = input.y;
  object.w = input.w;
  object.h = input.h;
  object.name = "person";
  object.cls = 0;
  object.confidence = 0.9f;
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
struct SquareZoneInput
{
  const char* kind{nullptr};
  float fromX{0};
  float fromY{0};
  float toX{0};
  float toY{0};
};

OperatorZone squareZone(const SquareZoneInput& input)
{
  OperatorZone zone;
  zone.name = input.kind;
  zone.kind = input.kind;
  zone.points = {{input.fromX, input.fromY},
                 {input.toX, input.fromY},
                 {input.toX, input.toY},
                 {input.fromX, input.toY}};
  zone.cameraId = 1;
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
  auto input = baseInput({personAt({.x = 280, .y = 200, .w = 80, .h = 160})});
  input.zones.push_back(squareZone({.kind = "exclude", .fromX = 0.25f,
            .fromY = 0.25f, .toX = 0.75f, .toY = 0.75f}));

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK_FALSE(outcome.publish);
  CHECK(outcome.rule == "exclude_zone");
}

TEST_CASE("rule 3: person inside an alert zone is critical")
{
  auto input = baseInput({personAt({.x = 280, .y = 200, .w = 80, .h = 160})});
  input.zones.push_back(squareZone({.kind = "alert", .fromX = 0.25f,
            .fromY = 0.25f, .toX = 0.75f, .toY = 0.75f}));

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "person_in_alert_zone");
  CHECK(outcome.severity == EventSeverity::Critical);
}

TEST_CASE("rule 4: person inside a monitor zone is warning")
{
  auto input = baseInput({personAt({.x = 280, .y = 200, .w = 80, .h = 160})});
  input.zones.push_back(squareZone({.kind = "monitor", .fromX = 0.25f,
            .fromY = 0.25f, .toX = 0.75f, .toY = 0.75f}));

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "person_in_monitor_zone");
  CHECK(outcome.severity == EventSeverity::Warning);
}

TEST_CASE("rules 5 and 6: person by the night schedule")
{
  auto night = baseInput({personAt({.x = 0, .y = 0, .w = 80, .h = 160})});
  night.night = true;
  const auto nightOutcome = EventIntelligence::evaluate(night);
  CHECK(nightOutcome.publish);
  CHECK(nightOutcome.rule == "person_night");
  CHECK(nightOutcome.severity == EventSeverity::Warning);

  const auto dayOutcome =
      EventIntelligence::evaluate(baseInput({personAt({.x = 0, .y = 0,
                                                       .w = 80, .h = 160})}));
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
  auto input = baseInput({personAt({.x = 280, .y = 200, .w = 80, .h = 160})});
  input.zones.push_back(squareZone({.kind = "alert", .fromX = 0.25f,
            .fromY = 0.25f, .toX = 0.75f, .toY = 0.75f}));
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
  auto input = baseInput({personAt({.x = 280, .y = 200, .w = 80, .h = 160})});
  input.zones.push_back(squareZone({.kind = "alert", .fromX = 0.25f,
            .fromY = 0.25f, .toX = 0.75f, .toY = 0.75f}));
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
  auto input = baseInput({vehicleAt(10, 10), personAt({.x = 0, .y = 0, .w = 80, .h = 160})});
  input.state.vehiclePreviouslyAbsent = true;

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "person_day");
}
