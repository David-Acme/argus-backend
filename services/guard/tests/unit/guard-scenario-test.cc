#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <ctime>
#include <doctest/doctest.h>
#include <feature/guard/guard-service.hxx>
#include <iostream>
#include <memory>
#include <string>
#include <text/json-util.hxx>
#include <vector>

#include "guard-fakes.hxx"

using guard_test::GuardBoot;
using guard_test::QuietCameraActions;
using guard_test::RecordingNotifications;
using guard_test::RosterIdentity;
using guard_test::SentNotification;

namespace
{

GuardBoot& boot()
{
  static GuardBoot shared("guard-scenario-test");
  return shared;
}

struct PersonEventInput
{
  std::string eventId;
  int64_t cameraId{0};
  std::string cameraName;
  int64_t trackId{0};
  int64_t personId{0};
  std::string rule;
  std::string severity;
  bool night{false};
  std::string zoneKind;
  std::string identityState;
};

Json::Value personEvent(const PersonEventInput& input)
{
  constexpr int64_t kPublishedAt = 1700000000000;
  Json::Value event(Json::objectValue);
  event["schemaVersion"] = 3;
  event["eventId"] = input.eventId;
  event["cameraId"] = Json::Int64(input.cameraId);
  event["cameraName"] = input.cameraName;
  event["rule"] = input.rule;
  event["severity"] = input.severity;
  event["escalated"] = false;
  event["night"] = input.night;
  event["trackId"] = Json::Int64(input.trackId);
  event["publishedAt"] = Json::Int64(kPublishedAt);
  Json::Value object(Json::objectValue);
  object["class"] = "person";
  object["confidence"] = 0.9;
  if (input.personId > 0) {
    object["personId"] = Json::Int64(input.personId);
    object["identity"] = "unknown";
  }
  object["identityState"] = input.identityState;
  object["identifyAttempts"] = 2;
  object["scoreMedian"] = 0.9;
  object["scoreSamples"] = 4;
  object["zoneWindows"] = 3;
  object["trackWindows"] = 4;
  object["areaSpread"] = 1.2;
  object["trackId"] = Json::Int64(input.trackId);
  object["firstSeenMs"] = Json::Int64(kPublishedAt - 18000);
  object["lastSeenMs"] = Json::Int64(kPublishedAt);
  object["dwellMs"] = Json::Int64(18000);
  if (!input.zoneKind.empty())
    object["zoneKind"] = input.zoneKind;
  object["observationId"] = std::to_string(input.cameraId) + ":" +
                            std::to_string(input.trackId) + ":1";
  object["bbox"] = Json::Value(Json::objectValue);
  Json::Value objects(Json::arrayValue);
  objects.append(object);
  event["objects"] = objects;
  return event;
}

GuardService::Config defaults()
{
  GuardService::Config config;
  config.enabled = true;
  config.profile = "home";
  config.defaultMode = GuardMode::Home;
  config.greetEnabled = false;
  config.greetReplyEnabled = false;
  return config;
}

struct CameraContextRow
{
  int64_t cameraId{0};
  std::string role;
  bool outdoor{false};
  bool publicArea{false};
};

void describeCamera(const CameraContextRow& row)
{
  try {
    DbService::client()->execSqlSync(
        "INSERT OR REPLACE INTO guard_camera_context (camera_id, role, "
        "outdoor, public_area, active_hours, updated_at) VALUES (?, ?, ?, ?, "
        "'', 0)",
        row.cameraId, row.role, row.outdoor ? 1 : 0, row.publicArea ? 1 : 0);
  }
  catch (const std::exception& error) {
    std::cout << "  (no camera context table: " << error.what() << ")\n";
  }
}

struct ScenarioInput
{
  std::string name;
  GuardService::Config config;
  std::vector<Json::Value> events;
};

struct ScenarioResult
{
  std::vector<SentNotification> sent;
  int announces{0};
  int alarms{0};
};

ScenarioResult play(const ScenarioInput& input)
{
  QuietCameraActions camera;
  RosterIdentity identity({{1, "es"}});
  RecordingNotifications notifications;
  GuardService service({.bus = nullptr,
                        .identity = &identity,
                        .notifications = &notifications,
                        .actions = &camera,
                        .assessment = nullptr},
                       input.config);
  for (const auto& event : input.events)
    REQUIRE(drogon::sync_wait(service.handle(event, 1)));
  ScenarioResult result{.sent = notifications.sent(),
                        .announces = camera.announces.load(),
                        .alarms = camera.alarms.load()};
  std::cout << "[scenario] " << input.name << ": " << input.events.size()
            << " observations -> " << result.sent.size()
            << " notification(s), " << result.announces
            << " spoken line(s), " << result.alarms << " alarm(s)\n";
  for (const auto& sent : result.sent)
    std::cout << "    title: " << sent.title << "\n    body:  " << sent.body
              << "\n";
  return result;
}

std::string windowAvoidingNow()
{
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  const auto clock = [](int hour) {
    const int wrapped = (hour % 24 + 24) % 24;
    return std::string(wrapped < 10 ? "0" : "") + std::to_string(wrapped) +
           ":00";
  };
  return clock(local.tm_hour + 2) + "-" + clock(local.tm_hour + 4);
}

}

TEST_CASE("home exterior at night: one calm alert for a stranger")
{
  (void)boot();
  describeCamera(
      {.cameraId = 301, .role = "perimeter", .outdoor = true, .publicArea = false});
  std::vector<Json::Value> events;
  for (int step = 1; step <= 4; ++step)
    events.push_back(personEvent({.eventId = "sc1:" + std::to_string(step),
                                  .cameraId = 301,
                                  .cameraName = "Jardín",
                                  .trackId = 1,
                                  .personId = 0,
                                  .rule = "person_night",
                                  .severity = "warning",
                                  .night = true,
                                  .zoneKind = {},
                                  .identityState = "unrecognized"}));
  const ScenarioResult result = play(
      {.name = "home exterior at night", .config = defaults(), .events = events});
  REQUIRE(result.sent.size() == 1);
  CHECK(result.sent.front().title == "Persona desconocida · Jardín");
  CHECK(result.sent.front().body ==
        "En el exterior, de noche, desde hace 18 s. Argus le está avisando "
        "por el altavoz.");
}

TEST_CASE("restaurant kitchen during service: staff never page the owner")
{
  (void)boot();
  describeCamera(
      {.cameraId = 302, .role = "kitchen", .outdoor = false, .publicArea = false});
  GuardService::Config config = defaults();
  config.profile = "commercial";
  config.schedule = {.enabled = true,
                     .asleep = {},
                     .open = "00:00-24:00",
                     .staffed = {},
                     .closedMode = "away"};
  std::vector<Json::Value> events;
  for (int step = 1; step <= 6; ++step)
    events.push_back(personEvent({.eventId = "sc2:" + std::to_string(step),
                                  .cameraId = 302,
                                  .cameraName = "Cocina",
                                  .trackId = 1,
                                  .personId = 0,
                                  .rule = "person_day",
                                  .severity = "info",
                                  .night = false,
                                  .zoneKind = {},
                                  .identityState = "unobservable"}));
  const ScenarioResult result =
      play({.name = "restaurant kitchen during service",
            .config = config,
            .events = events});
  CHECK(result.sent.empty());
  CHECK(result.announces == 0);
}

TEST_CASE("office after hours: the stranger is urgent and explained")
{
  (void)boot();
  describeCamera(
      {.cameraId = 303, .role = "office", .outdoor = false, .publicArea = false});
  GuardService::Config config = defaults();
  config.profile = "office";
  config.schedule = {.enabled = true,
                     .asleep = {},
                     .open = {},
                     .staffed = windowAvoidingNow(),
                     .closedMode = "away"};
  std::vector<Json::Value> events;
  for (int step = 1; step <= 3; ++step)
    events.push_back(personEvent({.eventId = "sc3:" + std::to_string(step),
                                  .cameraId = 303,
                                  .cameraName = "Oficina",
                                  .trackId = 1,
                                  .personId = 0,
                                  .rule = "person_day",
                                  .severity = "info",
                                  .night = false,
                                  .zoneKind = {},
                                  .identityState = "unrecognized"}));
  const ScenarioResult result = play(
      {.name = "office after hours", .config = config, .events = events});
  REQUIRE(result.sent.size() == 1);
  CHECK(result.sent.front().title == "Persona desconocida · Oficina");
  CHECK(result.sent.front().body ==
        "En la oficina, fuera de horario, desde hace 18 s. Argus le está "
        "avisando por el altavoz.");
  const Json::Value data = json_util::fromString(result.sent.front().data);
  CHECK(data["urgency"].asString() == "critical");
}

TEST_CASE("street camera at night: passers-by stay out of the phone")
{
  (void)boot();
  describeCamera(
      {.cameraId = 304, .role = "perimeter", .outdoor = true, .publicArea = true});
  std::vector<Json::Value> events;
  for (int person = 1; person <= 3; ++person)
    events.push_back(personEvent({.eventId = "sc4:" + std::to_string(person),
                                  .cameraId = 304,
                                  .cameraName = "Calle",
                                  .trackId = person,
                                  .personId = 9400 + person,
                                  .rule = "person_night",
                                  .severity = "warning",
                                  .night = true,
                                  .zoneKind = {},
                                  .identityState = "unrecognized"}));
  const ScenarioResult result = play(
      {.name = "street camera at night", .config = defaults(), .events = events});
  CHECK(result.sent.empty());
  CHECK(result.announces == 0);
}

TEST_CASE("quiet hours: a medium visit waits for the morning summary")
{
  (void)boot();
  describeCamera(
      {.cameraId = 305, .role = "entrance", .outdoor = true, .publicArea = false});
  GuardService::Config config = defaults();
  config.stagingEnabled = false;
  config.quietHoursEnabled = true;
  config.quietStartHour = 0;
  config.quietEndHour = 24;
  config.quietDailyBudget = 1000;
  const ScenarioResult result = play(
      {.name = "entrance during quiet hours",
       .config = config,
       .events = {personEvent({.eventId = "sc5:1",
                               .cameraId = 305,
                               .cameraName = "Entrada",
                               .trackId = 1,
                               .personId = 0,
                               .rule = "person_in_monitor_zone",
                               .severity = "warning",
                               .night = false,
                               .zoneKind = "monitor",
                               .identityState = "unrecognized"})}});
  CHECK(result.sent.empty());
}
