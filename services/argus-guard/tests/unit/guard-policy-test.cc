#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <guard-policy.hxx>
#include <shared/utils/base64/base64.hxx>

#include <vector>

namespace
{
GuardContext unknownPerson()
{
  GuardContext context;
  context.hasUnknown = true;
  context.rule = "person_day";
  return context;
}
} // namespace

TEST_CASE("a known person alone is safe")
{
  GuardContext context;
  context.hasKnown = true;
  context.rule = "known_person";
  CHECK(guard_policy::evaluate(context) == GuardDanger::None);
}

TEST_CASE("an unknown person at home is medium")
{
  CHECK(guard_policy::evaluate(unknownPerson()) == GuardDanger::Medium);
}

TEST_CASE("an unknown person while away is critical")
{
  auto context = unknownPerson();
  context.mode = GuardMode::Away;
  CHECK(guard_policy::evaluate(context) == GuardDanger::Critical);
}

TEST_CASE("an unknown person in an alert zone is critical")
{
  auto context = unknownPerson();
  context.inAlertZone = true;
  context.rule = "person_in_alert_zone";
  CHECK(guard_policy::evaluate(context) == GuardDanger::Critical);
}

TEST_CASE("an unknown person at night is high")
{
  auto context = unknownPerson();
  context.atNight = true;
  context.rule = "person_night";
  CHECK(guard_policy::evaluate(context) == GuardDanger::High);
}

TEST_CASE("an unknown companion of a trusted person is low")
{
  auto context = unknownPerson();
  context.hasKnown = true;
  context.trustedCompanion = true;
  CHECK(guard_policy::evaluate(context) == GuardDanger::Low);
}

TEST_CASE("an expected guest is low")
{
  auto context = unknownPerson();
  context.expectedGuest = true;
  CHECK(guard_policy::evaluate(context) == GuardDanger::Low);
}

TEST_CASE("a repeat visitor reaches high")
{
  auto context = unknownPerson();
  context.visitCount = 3;
  CHECK(guard_policy::evaluate(context) == GuardDanger::High);
}

TEST_CASE("a critical severity never lowers below high")
{
  auto context = unknownPerson();
  context.severity = "critical";
  CHECK(guard_policy::evaluate(context) == GuardDanger::High);
}

TEST_CASE("mode parsing round-trips")
{
  CHECK(guard_policy::modeFromString("away") == GuardMode::Away);
  CHECK(guard_policy::modeFromString("night") == GuardMode::Night);
  CHECK(guard_policy::modeFromString("armed") == GuardMode::Armed);
  CHECK(guard_policy::modeFromString("home") == GuardMode::Home);
  CHECK(guard_policy::modeFromString("bogus") == GuardMode::Home);
  CHECK(guard_policy::modeToString(GuardMode::Away) == "away");
}

TEST_CASE("the primary track alone decides known or unknown")
{
  Json::Value event(Json::objectValue);
  event["cameraId"] = 3;
  event["cameraName"] = "Front";
  event["rule"] = "person_day";
  Json::Value known(Json::objectValue);
  known["class"] = "person";
  known["identity"] = "known";
  known["personId"] = Json::Int64(7);
  Json::Value unknown(Json::objectValue);
  unknown["class"] = "person";
  unknown["identity"] = "unknown";
  unknown["personId"] = Json::Int64(12);
  Json::Value objects(Json::arrayValue);
  objects.append(known);
  objects.append(unknown);
  event["objects"] = objects;

  const auto signals = guard_policy::parseObjectEvent(event);
  CHECK(signals.hasKnown);
  CHECK_FALSE(signals.hasUnknown);
  CHECK(signals.knownPersonId == 7);
  CHECK(signals.personId == 7);

  Json::Value bbox(Json::objectValue);
  bbox["w"] = 40.0;
  bbox["h"] = 40.0;
  unknown["bbox"] = bbox;
  objects[1] = unknown;
  event["objects"] = objects;
  const auto largerUnknown = guard_policy::parseObjectEvent(event);
  CHECK_FALSE(largerUnknown.hasKnown);
  CHECK(largerUnknown.hasUnknown);
  CHECK(largerUnknown.personId == 12);
  CHECK(largerUnknown.knownPersonId == 0);
}

TEST_CASE("a known companion never shields an unknown primary")
{
  Json::Value event(Json::objectValue);
  event["cameraId"] = 3;
  event["rule"] = "person_in_alert_zone";
  event["severity"] = "critical";
  event["trackId"] = Json::Int64(22);
  Json::Value known(Json::objectValue);
  known["class"] = "person";
  known["identity"] = "known";
  known["personId"] = Json::Int64(7);
  known["trackId"] = Json::Int64(21);
  Json::Value bbox(Json::objectValue);
  bbox["w"] = 5.0;
  bbox["h"] = 5.0;
  known["bbox"] = bbox;
  Json::Value unknown(Json::objectValue);
  unknown["class"] = "person";
  unknown["identity"] = "unknown";
  unknown["personId"] = Json::Int64(0);
  unknown["trackId"] = Json::Int64(22);
  unknown["zoneKind"] = "alert";
  Json::Value bigBox(Json::objectValue);
  bigBox["w"] = 40.0;
  bigBox["h"] = 40.0;
  unknown["bbox"] = bigBox;
  Json::Value objects(Json::arrayValue);
  objects.append(known);
  objects.append(unknown);
  event["objects"] = objects;

  const auto signals = guard_policy::parseObjectEvent(event);
  CHECK(signals.hasUnknown);
  CHECK_FALSE(signals.hasKnown);
  CHECK(signals.personId == 0);
  CHECK(signals.trackId == 22);
  CHECK(signals.zoneKind == "alert");

  GuardContext context;
  context.rule = signals.rule;
  context.severity = signals.severity;
  context.hasKnown = signals.hasKnown;
  context.hasUnknown = signals.hasUnknown;
  context.inAlertZone = signals.zoneKind == "alert";
  CHECK(guard_policy::evaluate(context) == GuardDanger::Critical);
}

TEST_CASE("a known person alone propagates its person id")
{
  Json::Value event(Json::objectValue);
  event["cameraId"] = 3;
  event["rule"] = "known_person";
  Json::Value known(Json::objectValue);
  known["class"] = "person";
  known["identity"] = "known";
  known["personId"] = Json::Int64(7);
  Json::Value objects(Json::arrayValue);
  objects.append(known);
  event["objects"] = objects;

  const auto signals = guard_policy::parseObjectEvent(event);
  CHECK(signals.hasKnown);
  CHECK_FALSE(signals.hasUnknown);
  CHECK(signals.knownPersonId == 7);
  CHECK(signals.personId == 7);
}

TEST_CASE("a person without identity fields counts as unknown")
{
  Json::Value event(Json::objectValue);
  event["cameraId"] = 3;
  event["rule"] = "person_day";
  Json::Value person(Json::objectValue);
  person["class"] = "person";
  Json::Value objects(Json::arrayValue);
  objects.append(person);
  event["objects"] = objects;

  const auto signals = guard_policy::parseObjectEvent(event);
  CHECK_FALSE(signals.hasKnown);
  CHECK(signals.hasUnknown);
  CHECK(signals.personId == 0);
}

TEST_CASE("a person rule without objects still counts as unknown")
{
  Json::Value event(Json::objectValue);
  event["cameraId"] = 3;
  event["rule"] = "person_night";

  const auto signals = guard_policy::parseObjectEvent(event);
  CHECK(signals.hasUnknown);
}

TEST_CASE("a vehicle event carries no person signals")
{
  Json::Value event(Json::objectValue);
  event["cameraId"] = 3;
  event["rule"] = "vehicle_night";
  Json::Value vehicle(Json::objectValue);
  vehicle["class"] = "car";
  Json::Value objects(Json::arrayValue);
  objects.append(vehicle);
  event["objects"] = objects;

  const auto signals = guard_policy::parseObjectEvent(event);
  CHECK_FALSE(signals.hasKnown);
  CHECK_FALSE(signals.hasUnknown);
}

TEST_CASE("cross-camera correlation matches the same identity")
{
  GuardEncounterCandidate candidate;
  candidate.id = 4;
  candidate.personId = 9;
  candidate.signature = "AAAA";
  candidate.lastSeen = 100;
  const std::vector<GuardEncounterCandidate> candidates{candidate};

  const auto matched = guard_policy::matchEncounter(
      {.personId = 9,
       .signature = "BBBB",
       .now = 110,
       .windowS = 60,
       .minSimilarity = 0.8,
       .candidates = candidates});
  REQUIRE(matched);
  CHECK(*matched == 4);
}

namespace
{
std::string histogramSignature(const std::vector<int>& bins)
{
  std::string bytes(bins.size(), '\0');
  for (size_t i = 0; i < bins.size(); ++i)
    bytes[i] = static_cast<char>(bins[i]);
  return base64::encode(bytes);
}
} // namespace

TEST_CASE("cross-camera correlation falls back to the appearance signature")
{
  const std::string same = histogramSignature(std::vector<int>(24, 255));
  std::vector<int> distinct(24, 0);
  for (int i = 0; i < 8; ++i)
    distinct[i] = 255;
  const std::string other = histogramSignature(distinct);
  GuardEncounterCandidate candidate;
  candidate.id = 7;
  candidate.signature = same;
  candidate.lastSeen = 100;
  const std::vector<GuardEncounterCandidate> candidates{candidate};

  const auto matched = guard_policy::matchEncounter(
      {.personId = 0,
       .signature = same,
       .now = 110,
       .windowS = 60,
       .minSimilarity = 0.8,
       .candidates = candidates});
  REQUIRE(matched);
  CHECK(*matched == 7);

  const auto rejected = guard_policy::matchEncounter(
      {.personId = 0,
       .signature = other,
       .now = 110,
       .windowS = 60,
       .minSimilarity = 0.8,
       .candidates = candidates});
  CHECK_FALSE(rejected);
}

TEST_CASE("cross-camera correlation ignores stale encounters")
{
  GuardEncounterCandidate candidate;
  candidate.id = 1;
  candidate.signature = histogramSignature(std::vector<int>(24, 255));
  candidate.lastSeen = 10;
  const std::vector<GuardEncounterCandidate> candidates{candidate};

  const auto matched = guard_policy::matchEncounter(
      {.personId = 0,
       .signature = candidate.signature,
       .now = 200,
       .windowS = 60,
       .minSimilarity = 0.8,
       .candidates = candidates});
  CHECK_FALSE(matched);
}

TEST_CASE("the event parser reads dwell, track and signature")
{
  Json::Value event(Json::objectValue);
  event["cameraId"] = 6;
  event["rule"] = "person_day";
  event["dwellMs"] = Json::Int64(13000);
  event["trackId"] = Json::Int64(3);
  Json::Value person(Json::objectValue);
  person["class"] = "person";
  person["signature"] = "AAAA";
  Json::Value bbox(Json::objectValue);
  bbox["w"] = 200.0;
  bbox["h"] = 500.0;
  person["bbox"] = bbox;
  Json::Value objects(Json::arrayValue);
  objects.append(person);
  event["objects"] = objects;

  const auto signals = guard_policy::parseObjectEvent(event);
  CHECK(signals.dwellMs == 13000);
  CHECK(signals.trackId == 3);
  CHECK(signals.signature == "AAAA");
  CHECK(signals.viewScore == doctest::Approx(100000.0));
}

TEST_CASE("cross-camera correlation keeps same-camera continuity")
{
  const std::string previous = histogramSignature(std::vector<int>(24, 255));
  std::vector<int> distinct(24, 0);
  for (int i = 0; i < 8; ++i)
    distinct[i] = 255;
  const std::string current = histogramSignature(distinct);

  GuardEncounterCandidate candidate;
  candidate.id = 5;
  candidate.signature = previous;
  candidate.lastCameraId = 6;
  candidate.lastSeen = 100;
  const std::vector<GuardEncounterCandidate> candidates{candidate};

  const auto matched = guard_policy::matchEncounter(
      {.personId = 0,
       .signature = current,
       .cameraId = 6,
       .now = 110,
       .windowS = 60,
       .continuityWindowS = 20,
       .minSimilarity = 0.8,
       .candidates = candidates});
  REQUIRE(matched);
  CHECK(*matched == 5);

  const auto otherCamera = guard_policy::matchEncounter(
      {.personId = 0,
       .signature = current,
       .cameraId = 7,
       .now = 110,
       .windowS = 60,
       .continuityWindowS = 20,
       .minSimilarity = 0.8,
       .candidates = candidates});
  CHECK_FALSE(otherCamera);
}

TEST_CASE("greeting variants rotate by encounter and stay empty when unset")
{
  const std::vector<std::string> variants{"one", "two", "three"};
  CHECK(guard_policy::pickGreeting(4, variants) == "two");
  CHECK(guard_policy::pickGreeting(5, variants) == "three");
  CHECK(guard_policy::pickGreeting(6, variants) == "one");
  CHECK(guard_policy::pickGreeting(-7, variants) == "two");
  CHECK(guard_policy::pickGreeting(3, {}) == "");
}
