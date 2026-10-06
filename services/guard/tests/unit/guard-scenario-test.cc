#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <array>
#include <ctime>
#include <doctest/doctest.h>
#include <feature/guard/guard-service.hxx>
#include <feature/guard/repositories/environment/environment-repository.hxx>
#include <iostream>
#include <atomic>
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
using guard_test::scalar;

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
  config.staleObservationS = 0;
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
  int64_t environmentId{1};
};

void describeCamera(const CameraContextRow& row)
{
  DbService::client()->execSqlSync(
      "INSERT OR REPLACE INTO guard_camera_context (camera_id, role, "
      "outdoor, public_area, active_hours, environment_id, updated_at) "
      "VALUES (?, ?, ?, ?, '', ?, 0)",
      row.cameraId, row.role, row.outdoor ? 1 : 0, row.publicArea ? 1 : 0,
      row.environmentId);
}

struct EnvironmentSpec
{
  std::string name;
  EnvironmentKind kind{EnvironmentKind::Home};
  GuardMode mode{GuardMode::Home};
  bool scheduleEnabled{false};
  std::string asleep;
  std::string open;
  std::string staffed;
  int digestHour{-1};
};

int64_t environment(const EnvironmentSpec& spec)
{
  const EnvironmentRepository repository;
  for (const auto& existing : drogon::sync_wait(repository.list())) {
    if (existing.name == spec.name)
      return existing.id;
  }
  return drogon::sync_wait(repository.create({.name = spec.name,
                                              .kind = spec.kind,
                                              .mode = spec.mode,
                                              .scheduleEnabled = spec.scheduleEnabled,
                                              .asleep = spec.asleep,
                                              .open = spec.open,
                                              .staffed = spec.staffed,
                                              .closedMode = GuardMode::Away,
                                              .digestHour = spec.digestHour,
                                              .quietPolicy = QuietPolicy::Inherit,
                                              .quietStartHour = 22,
                                              .quietEndHour = 7,
                                              .lanPresence = false,
                                              .at = 1}))
      .id;
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

std::string clockAt(int hour)
{
  const int wrapped = (hour % 24 + 24) % 24;
  return std::string(wrapped < 10 ? "0" : "") + std::to_string(wrapped) + ":00";
}

int currentHour()
{
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  return local.tm_hour;
}

std::string windowAvoidingNow()
{
  return clockAt(currentHour() + 2) + "-" + clockAt(currentHour() + 4);
}

std::string windowCoveringNow()
{
  return clockAt(currentHour() - 1) + "-" + clockAt(currentHour() + 2);
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
  const int64_t restaurant = environment({.name = "Trattoria",
                                         .kind = EnvironmentKind::Restaurant,
                                         .mode = GuardMode::Home,
                                         .scheduleEnabled = true,
                                         .asleep = {},
                                         .open = "00:00-24:00",
                                         .staffed = {},
                                         .digestHour = -1});
  describeCamera({.cameraId = 302,
                  .role = "kitchen",
                  .outdoor = false,
                  .publicArea = false,
                  .environmentId = restaurant});
  const GuardService::Config config = defaults();
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
  const int64_t office = environment({.name = "Oficina Centro",
                                     .kind = EnvironmentKind::Office,
                                     .mode = GuardMode::Home,
                                     .scheduleEnabled = true,
                                     .asleep = {},
                                     .open = {},
                                     .staffed = windowAvoidingNow(),
                                     .digestHour = -1});
  describeCamera({.cameraId = 303,
                  .role = "office",
                  .outdoor = false,
                  .publicArea = false,
                  .environmentId = office});
  const GuardService::Config config = defaults();
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
  CHECK(result.sent.front().title ==
        "Persona desconocida · Oficina (Oficina Centro)");
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

TEST_CASE("three environments at the same moment: each camera is judged by its own place")
{
  (void)boot();
  const int64_t restaurant = environment({.name = "Trattoria",
                                         .kind = EnvironmentKind::Restaurant,
                                         .mode = GuardMode::Home,
                                         .scheduleEnabled = true,
                                         .asleep = {},
                                         .open = "00:00-24:00",
                                         .staffed = {},
                                         .digestHour = -1});
  const int64_t cottage = environment({.name = "Casa de campo",
                                      .kind = EnvironmentKind::Home,
                                      .mode = GuardMode::Home,
                                      .scheduleEnabled = true,
                                      .asleep = windowCoveringNow(),
                                      .open = {},
                                      .staffed = {},
                                      .digestHour = -1});
  const int64_t office = environment({.name = "Oficina Centro",
                                     .kind = EnvironmentKind::Office,
                                     .mode = GuardMode::Home,
                                     .scheduleEnabled = true,
                                     .asleep = {},
                                     .open = {},
                                     .staffed = windowAvoidingNow(),
                                     .digestHour = -1});
  describeCamera({.cameraId = 311,
                  .role = "kitchen",
                  .outdoor = false,
                  .publicArea = false,
                  .environmentId = restaurant});
  describeCamera({.cameraId = 312,
                  .role = "perimeter",
                  .outdoor = true,
                  .publicArea = false,
                  .environmentId = cottage});
  describeCamera({.cameraId = 313,
                  .role = "office",
                  .outdoor = false,
                  .publicArea = false,
                  .environmentId = office});
  std::vector<Json::Value> events;
  for (int step = 1; step <= 6; ++step) {
    events.push_back(personEvent({.eventId = "mix:kitchen:" + std::to_string(step),
                                  .cameraId = 311,
                                  .cameraName = "Cocina",
                                  .trackId = 1,
                                  .personId = 0,
                                  .rule = "person_day",
                                  .severity = "info",
                                  .night = false,
                                  .zoneKind = {},
                                  .identityState = "unobservable"}));
    if (step <= 4)
      events.push_back(personEvent({.eventId = "mix:garden:" + std::to_string(step),
                                    .cameraId = 312,
                                    .cameraName = "Jardín",
                                    .trackId = 1,
                                    .personId = 0,
                                    .rule = "person_day",
                                    .severity = "info",
                                    .night = false,
                                    .zoneKind = {},
                                    .identityState = "unrecognized"}));
    if (step <= 3)
      events.push_back(personEvent({.eventId = "mix:office:" + std::to_string(step),
                                    .cameraId = 313,
                                    .cameraName = "Oficina",
                                    .trackId = 1,
                                    .personId = 0,
                                    .rule = "person_day",
                                    .severity = "info",
                                    .night = false,
                                    .zoneKind = {},
                                    .identityState = "unrecognized"}));
  }
  const ScenarioResult result = play(
      {.name = "restaurant open + home asleep + office closed, interleaved",
       .config = defaults(),
       .events = events});
  REQUIRE(result.sent.size() == 2);
  std::vector<std::string> titles;
  for (const auto& sent : result.sent) {
    titles.push_back(sent.title);
    const Json::Value data = json_util::fromString(sent.data);
    CHECK(data["environmentId"].asInt64() != restaurant);
    CHECK(data["environmentName"].asString() != "Trattoria");
  }
  CHECK(std::ranges::find(titles, "Persona desconocida · Jardín (Casa de campo)") !=
        titles.end());
  CHECK(std::ranges::find(titles, "Persona desconocida · Oficina (Oficina Centro)") !=
        titles.end());
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE "
               "event_id LIKE 'mix:kitchen:%' AND environment_id = " +
               std::to_string(restaurant)) == "6");
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE "
               "event_id LIKE 'mix:%' AND did_notify = 1 AND environment_id = " +
               std::to_string(restaurant)) == "0");
  CHECK(scalar("SELECT COUNT(DISTINCT environment_id) FROM guard_encounter "
               "WHERE best_camera_id IN (311, 312, 313)") == "3");
}

TEST_CASE("one summary per environment and per day, each in its own thread")
{
  (void)boot();
  const int hour = currentHour();
  DbService::client()->execSqlSync(
      "UPDATE guard_environment SET digest_hour = ?", hour);
  QuietCameraActions camera;
  RosterIdentity identity({{1, "es"}});
  RecordingNotifications notifications;
  GuardService service({.bus = nullptr,
                        .identity = &identity,
                        .notifications = &notifications,
                        .actions = &camera,
                        .assessment = nullptr},
                       defaults());
  const int64_t now = static_cast<int64_t>(std::time(nullptr)) + 1;
  drogon::sync_wait(service.maybeSendDigests(now));
  const auto sent = notifications.sent();
  std::cout << "[scenario] daily summaries: " << sent.size() << "\n";
  for (const auto& digest : sent)
    std::cout << "    title: " << digest.title << "\n    body:  " << digest.body
              << "\n";
  REQUIRE(sent.size() >= 2);
  std::array<char, 16> day{};
  const auto at = static_cast<std::time_t>(now);
  std::tm local{};
  localtime_r(&at, &local);
  const std::string today(day.data(),
                          std::strftime(day.data(), day.size(), "%Y-%m-%d", &local));
  std::vector<std::string> threads;
  bool trattoria = false;
  for (const auto& digest : sent) {
    const Json::Value data = json_util::fromString(digest.data);
    CHECK(data["kind"].asString() == "guard_digest");
    const std::string thread = data["threadKey"].asString();
    CHECK(thread == "guard:digest:" + std::to_string(data["environmentId"].asInt64()) +
                        ":" + today);
    threads.push_back(thread);
    trattoria = trattoria || digest.title == "Resumen de vigilancia · Trattoria";
  }
  CHECK(trattoria);
  std::ranges::sort(threads);
  CHECK(std::ranges::adjacent_find(threads) == threads.end());
  drogon::sync_wait(service.maybeSendDigests(now + 60));
  CHECK(notifications.sent().size() == sent.size());
  DbService::client()->execSqlSync("UPDATE guard_environment SET digest_hour = -1");
}

TEST_CASE("daily summaries stay silent while surveillance is disabled and resume with it")
{
  (void)boot();
  DbService::client()->execSqlSync("DELETE FROM guard_state WHERE key LIKE 'digest_%'");
  DbService::client()->execSqlSync("UPDATE guard_environment SET digest_hour = ?", currentHour());
  QuietCameraActions camera;
  RosterIdentity identity({{1, "es"}});
  RecordingNotifications notifications;
  auto active = std::make_shared<std::atomic<bool>>(false);
  GuardService service({.bus = nullptr,
                        .identity = &identity,
                        .notifications = &notifications,
                        .actions = &camera,
                        .assessment = nullptr,
                        .directory = {},
                        .active = [active] { return active->load(); }},
                       defaults());
  const int64_t now = static_cast<int64_t>(std::time(nullptr)) + 1;

  drogon::sync_wait(service.maybeSendDigests(now));
  CHECK(notifications.sent().empty());
  CHECK(scalar("SELECT COUNT(*) FROM guard_state WHERE key LIKE 'digest_daily_day_%'") == "0");

  active->store(true);
  drogon::sync_wait(service.maybeSendDigests(now));
  CHECK(scalar("SELECT COUNT(*) FROM guard_state WHERE key LIKE 'digest_daily_day_%'") != "0");
  DbService::client()->execSqlSync("UPDATE guard_environment SET digest_hour = -1");
}

TEST_CASE("guard stops evaluating while surveillance is disabled and resumes on enable")
{
  (void)boot();
  describeCamera(
      {.cameraId = 391, .role = "perimeter", .outdoor = true, .publicArea = false});
  const auto strangerAt = [](const std::string& prefix) {
    std::vector<Json::Value> events;
    for (int step = 1; step <= 4; ++step)
      events.push_back(personEvent({.eventId = prefix + std::to_string(step),
                                    .cameraId = 391,
                                    .cameraName = "Patio",
                                    .trackId = 1,
                                    .personId = 0,
                                    .rule = "person_night",
                                    .severity = "warning",
                                    .night = true,
                                    .zoneKind = {},
                                    .identityState = "unrecognized"}));
    return events;
  };

  QuietCameraActions camera;
  RosterIdentity identity({{1, "es"}});
  RecordingNotifications notifications;
  auto active = std::make_shared<std::atomic<bool>>(false);
  GuardService service({.bus = nullptr,
                        .identity = &identity,
                        .notifications = &notifications,
                        .actions = &camera,
                        .assessment = nullptr,
                        .directory = {},
                        .active = [active] { return active->load(); }},
                       defaults());
  CHECK_FALSE(service.evaluating());
  for (const auto& event : strangerAt("gate-off:"))
    CHECK(drogon::sync_wait(service.handle(event, 1)));
  CHECK(notifications.sent().empty());
  CHECK(camera.announces.load() == 0);

  active->store(true);
  CHECK(service.evaluating());
  for (const auto& event : strangerAt("gate-on:"))
    CHECK(drogon::sync_wait(service.handle(event, 1)));
  CHECK(notifications.sent().size() == 1);
}
