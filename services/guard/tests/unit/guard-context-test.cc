#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <feature/guard/guard-context.hxx>
#include <feature/guard/guard-policy.hxx>

#include <algorithm>
#include <ctime>

namespace
{
std::tm at(int weekday, int hour)
{
  std::tm local{};
  local.tm_wday = weekday;
  local.tm_hour = hour;
  local.tm_min = 0;
  return local;
}

GuardCameraContext camera(CameraRole role, bool outdoor, bool publicArea)
{
  return {.cameraId = 1,
          .role = role,
          .outdoor = outdoor,
          .publicArea = publicArea,
          .activeHours = {},
          .configured = true,
          .updatedAt = 0};
}

const GuardPosture kHome{.mode = GuardMode::Home,
                         .publicPresent = false,
                         .staffOnly = false,
                         .occupancy = "manual"};

GuardContext unknownAt(GuardMode mode)
{
  GuardContext context;
  context.mode = mode;
  context.hasUnknown = true;
  context.severity = "warning";
  return context;
}

bool has(const std::vector<GuardReason>& reasons, GuardReason reason)
{
  return std::ranges::find(reasons, reason) != reasons.end();
}
}

TEST_CASE("an unconfigured camera changes nothing")
{
  GuardCameraContext blank;
  const GuardArea area = guard_context::evaluate(
      {.camera = blank, .posture = kHome, .local = at(2, 12), .inAlertZone = false});
  CHECK_FALSE(area.inUse);
  CHECK_FALSE(area.passerby);
}

TEST_CASE("work areas are in use while staff or customers are in")
{
  const GuardPosture staffed{.mode = GuardMode::Home,
                             .publicPresent = false,
                             .staffOnly = true,
                             .occupancy = "staffed"};
  const auto kitchen = camera(CameraRole::Kitchen, false, false);
  CHECK(guard_context::evaluate({.camera = kitchen,
                                 .posture = staffed,
                                 .local = at(2, 9),
                                 .inAlertZone = false})
            .inUse);
  const auto entrance = camera(CameraRole::Entrance, true, false);
  CHECK_FALSE(guard_context::evaluate({.camera = entrance,
                                       .posture = staffed,
                                       .local = at(2, 9),
                                       .inAlertZone = false})
                  .inUse);
  CHECK_FALSE(guard_context::evaluate({.camera = kitchen,
                                       .posture = kHome,
                                       .local = at(2, 9),
                                       .inAlertZone = false})
                  .inUse);
}

TEST_CASE("a camera's own hours count at home, never when away or armed")
{
  auto office = camera(CameraRole::Office, false, false);
  office.activeHours = "mon-fri 09:00-18:00";
  CHECK(guard_context::evaluate(
            {.camera = office, .posture = kHome, .local = at(2, 10), .inAlertZone = false})
            .inUse);
  CHECK_FALSE(guard_context::evaluate({.camera = office,
                                       .posture = kHome,
                                       .local = at(6, 10),
                                       .inAlertZone = false})
                  .inUse);
  const GuardPosture away{.mode = GuardMode::Away,
                          .publicPresent = false,
                          .staffOnly = false,
                          .occupancy = "manual"};
  CHECK_FALSE(guard_context::evaluate({.camera = office,
                                       .posture = away,
                                       .local = at(2, 10),
                                       .inAlertZone = false})
                  .inUse);
  const GuardPosture closed{.mode = GuardMode::Away,
                            .publicPresent = false,
                            .staffOnly = false,
                            .occupancy = "closed"};
  CHECK(guard_context::evaluate({.camera = office,
                                 .posture = closed,
                                 .local = at(2, 10),
                                 .inAlertZone = false})
            .inUse);
}

TEST_CASE("a public outdoor camera sees passers-by unless they enter a zone")
{
  const auto street = camera(CameraRole::Perimeter, true, true);
  CHECK(guard_context::evaluate(
            {.camera = street, .posture = kHome, .local = at(3, 2), .inAlertZone = false})
            .passerby);
  CHECK_FALSE(guard_context::evaluate({.camera = street,
                                       .posture = kHome,
                                       .local = at(3, 2),
                                       .inAlertZone = true})
                  .passerby);
}

TEST_CASE("expected activity is low and an owner-drawn zone stays medium")
{
  GuardContext context = unknownAt(GuardMode::Home);
  context.atNight = true;
  context.areaInUse = true;
  CHECK(guard_policy::evaluate(context) == GuardDanger::Low);
  context.inAlertZone = true;
  CHECK(guard_policy::evaluate(context) == GuardDanger::Medium);
  CHECK(has(guard_policy::explain(context), GuardReason::AreaInUse));
}

TEST_CASE("passers-by stay low even at night or away, repeat visits are medium")
{
  GuardContext context = unknownAt(GuardMode::Away);
  context.atNight = true;
  context.passerby = true;
  CHECK(guard_policy::evaluate(context) == GuardDanger::Low);
  context.visitCount = 3;
  CHECK(guard_policy::evaluate(context) == GuardDanger::Medium);
  const auto reasons = guard_policy::explain(context);
  CHECK(has(reasons, GuardReason::Passerby));
  CHECK(has(reasons, GuardReason::RepeatVisits));
}

TEST_CASE("explanations name the floor that raised the danger")
{
  GuardContext context = unknownAt(GuardMode::Away);
  context.afterHours = true;
  context.atNight = true;
  context.inAlertZone = true;
  const auto reasons = guard_policy::explain(context);
  CHECK(reasons.front() == GuardReason::AfterHours);
  CHECK(has(reasons, GuardReason::Night));
  CHECK(has(reasons, GuardReason::AlertZone));
  CHECK_FALSE(has(reasons, GuardReason::NobodyHome));
  context.afterHours = false;
  CHECK(guard_policy::explain(context).front() == GuardReason::NobodyHome);
}

TEST_CASE("a quiet area keeps the speaker and the siren silent")
{
  const GuardDeterrence loud = guard_policy::deterrence(
      {.mode = GuardMode::Armed,
       .danger = GuardDanger::Critical,
       .publicPresent = false,
       .staffOnly = false,
       .weapon = false,
       .inAlertZone = false,
       .encounterChecks = 1,
       .quietArea = false});
  CHECK(loud.voice);
  CHECK(loud.alarm);
  const GuardDeterrence quiet = guard_policy::deterrence(
      {.mode = GuardMode::Armed,
       .danger = GuardDanger::Critical,
       .publicPresent = false,
       .staffOnly = false,
       .weapon = false,
       .inAlertZone = false,
       .encounterChecks = 1,
       .quietArea = true});
  CHECK_FALSE(quiet.voice);
  CHECK_FALSE(quiet.alarm);
}

TEST_CASE("window specs are validated strictly")
{
  CHECK(guard_schedule::validWindows(""));
  CHECK(guard_schedule::validWindows("mon-fri 08:00-18:00, sat 10:00-14:00"));
  CHECK(guard_schedule::validWindows("22:00-06:00"));
  CHECK_FALSE(guard_schedule::validWindows("mon-fri 08:00-18:00, nope"));
  CHECK_FALSE(guard_schedule::validWindows("25:00-26:00"));
  CHECK_FALSE(guard_schedule::validWindows("08:00-08:00"));
}
