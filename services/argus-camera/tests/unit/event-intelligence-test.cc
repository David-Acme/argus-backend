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
  for (const auto& object : input.objects) {
    if (object.name == "person")
      input.persons.push_back(
          {.trackId = object.trackId, .due = true, .canEmit = true});
  }
  return input;
}

// A stub matcher that matches every person crop as person 7.
class KnownPerson7Matcher final : public IKnownPersonMatcher
{
public:
  std::optional<PersonMatch> match(const PersonCrop&) const override
  {
    return PersonMatch{.identity = PersonIdentity::Known,
                       .state = IdentityState::Known,
                       .personId = 7,
                       .confidence = 0.9F,
                       .identifyAttempts = 1};
  }
};

// A stub matcher that enrolls every person crop as an unknown person 9.
class UnknownPerson9Matcher final : public IKnownPersonMatcher
{
public:
  std::optional<PersonMatch> match(const PersonCrop&) const override
  {
    return PersonMatch{.identity = PersonIdentity::Unknown,
                       .state = IdentityState::Unrecognized,
                       .personId = 9,
                       .confidence = 0.8F,
                       .identifyAttempts = 2};
  }
};
} // namespace

TEST_CASE("a person below the dwell gate never reaches the person rules")
{
  auto input = baseInput({personAt({.x = 280, .y = 200, .w = 80, .h = 160})});
  for (auto& verdict : input.persons)
    verdict.due = false;

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK_FALSE(outcome.publish);
}

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

TEST_CASE("an enrolled unknown keeps the zone severity and carries its person id")
{
  UnknownPerson9Matcher matcher;
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
  REQUIRE(outcome.objects.size() == 1);
  CHECK(outcome.objects.front().personId.has_value());
  CHECK(*outcome.objects.front().personId == 9);
  CHECK_FALSE(outcome.objects.front().known);
}

TEST_CASE("a vehicle arriving before a person keeps the person rule dominant")
{
  auto input = baseInput({vehicleAt(10, 10), personAt({.x = 0, .y = 0, .w = 80, .h = 160})});
  input.state.vehiclePreviouslyAbsent = true;

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "person_day");
}

TEST_CASE("a companion in an exclude zone cannot veto the primary")
{
  DetectedObject primary = personAt({.x = 0, .y = 0, .w = 64, .h = 64});
  primary.trackId = 1;
  DetectedObject companion = personAt({.x = 400, .y = 0, .w = 64, .h = 64});
  companion.trackId = 2;

  auto input = baseInput({primary, companion});
  input.primaryTrackId = 1;
  input.zones.push_back(squareZone({.kind = "alert", .fromX = 0.0f,
            .fromY = 0.0f, .toX = 0.2f, .toY = 0.3f}));
  input.zones.push_back(squareZone({.kind = "exclude", .fromX = 0.5f,
            .fromY = 0.0f, .toX = 0.8f, .toY = 0.3f}));

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "person_in_alert_zone");
  CHECK(outcome.severity == EventSeverity::Critical);
}

TEST_CASE("the primary's zone decides even when a companion is in alert")
{
  DetectedObject primary = personAt({.x = 0, .y = 0, .w = 64, .h = 64});
  primary.trackId = 1;
  DetectedObject companion = personAt({.x = 400, .y = 0, .w = 64, .h = 64});
  companion.trackId = 2;

  auto input = baseInput({primary, companion});
  input.primaryTrackId = 1;
  input.zones.push_back(squareZone({.kind = "monitor", .fromX = 0.0f,
            .fromY = 0.0f, .toX = 0.2f, .toY = 0.3f}));
  input.zones.push_back(squareZone({.kind = "alert", .fromX = 0.5f,
            .fromY = 0.0f, .toX = 0.8f, .toY = 0.3f}));

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "person_in_monitor_zone");
  CHECK(outcome.severity == EventSeverity::Warning);
}

TEST_CASE("legacy frames without a primary keep the frame-wide exclude")
{
  DetectedObject person = personAt({.x = 0, .y = 0, .w = 64, .h = 64});
  person.trackId = 1;
  DetectedObject companion = personAt({.x = 400, .y = 0, .w = 64, .h = 64});
  companion.trackId = 2;

  auto input = baseInput({person, companion});
  input.primaryTrackId = 0;
  input.zones.push_back(squareZone({.kind = "exclude", .fromX = 0.5f,
            .fromY = 0.0f, .toX = 0.8f, .toY = 0.3f}));

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK_FALSE(outcome.publish);
  CHECK(outcome.rule == "exclude_zone");
}

// Recognizes only the right-hand companion crop as known person 9.
class RightCompanionKnownMatcher final : public IKnownPersonMatcher
{
public:
  std::optional<PersonMatch> match(const PersonCrop& crop) const override
  {
    if (crop.x <= 300.0F)
      return std::nullopt;
    return PersonMatch{.identity = PersonIdentity::Known,
                       .personId = 9,
                       .confidence = 0.9F};
  }
};

TEST_CASE("an unknown primary intruder ignores a known companion in exclude")
{
  DetectedObject primary = personAt({.x = 0, .y = 0, .w = 64, .h = 64});
  primary.trackId = 1;
  DetectedObject companion = personAt({.x = 400, .y = 0, .w = 64, .h = 64});
  companion.trackId = 2;

  RightCompanionKnownMatcher matcher;
  auto input = baseInput({primary, companion});
  input.primaryTrackId = 1;
  input.matcher = &matcher;
  static std::vector<uint8_t> frame(640 * 480 * 3, 128);
  input.frameRgb = frame.data();
  input.zones.push_back(squareZone({.kind = "alert", .fromX = 0.0f,
            .fromY = 0.0f, .toX = 0.2f, .toY = 0.3f}));
  input.zones.push_back(squareZone({.kind = "exclude", .fromX = 0.5f,
            .fromY = 0.0f, .toX = 0.8f, .toY = 0.3f}));

  const auto outcome = EventIntelligence::evaluate(input);
  CHECK(outcome.publish);
  CHECK(outcome.rule == "person_in_alert_zone");
  CHECK(outcome.severity == EventSeverity::Critical);
  CHECK_FALSE(outcome.knownPersonId.has_value());
  bool companionIdentified = false;
  bool primaryUnknown = false;
  for (const auto& entry : outcome.objects) {
    if (entry.object.trackId == 2 && entry.personId.has_value())
      companionIdentified = true;
    if (entry.object.trackId == 1 && !entry.known)
      primaryUnknown = true;
  }
  CHECK(companionIdentified);
  CHECK(primaryUnknown);
}

TEST_CASE("the identity tri-state reaches the evaluated objects")
{
  static std::vector<uint8_t> frame(640 * 480 * 3, 128);

  {
    UnknownPerson9Matcher matcher;
    auto input = baseInput({personAt({.x = 280, .y = 200, .w = 80, .h = 160})});
    input.matcher = &matcher;
    input.frameRgb = frame.data();
    const auto outcome = EventIntelligence::evaluate(input);
    REQUIRE(outcome.objects.size() == 1);
    CHECK(outcome.objects.front().identityState == "unrecognized");
    CHECK(outcome.objects.front().identifyAttempts == 2);
  }

  {
    KnownPerson7Matcher matcher;
    auto input = baseInput({personAt({.x = 280, .y = 200, .w = 80, .h = 160})});
    input.matcher = &matcher;
    input.frameRgb = frame.data();
    const auto outcome = EventIntelligence::evaluate(input);
    REQUIRE(outcome.objects.size() == 1);
    CHECK(outcome.objects.front().identityState == "known");
    CHECK(outcome.objects.front().identifyAttempts == 1);
  }

  {
    NoKnownPersonMatcher matcher;
    auto input = baseInput({personAt({.x = 280, .y = 200, .w = 80, .h = 160})});
    input.matcher = &matcher;
    input.frameRgb = frame.data();
    const auto outcome = EventIntelligence::evaluate(input);
    REQUIRE(outcome.objects.size() == 1);
    CHECK(outcome.objects.front().identityState.empty());
    CHECK(outcome.objects.front().identifyAttempts == 0);
  }
}
