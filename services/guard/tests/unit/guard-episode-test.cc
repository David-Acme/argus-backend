#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <ctime>
#include <doctest/doctest.h>
#include <feature/guard/dtos/update-camera-context-dto.hxx>
#include <feature/guard/dtos/update-guard-site-dto.hxx>
#include <feature/guard/guard-service.hxx>
#include <feature/guard/services/guard-feature-service.hxx>
#include <memory>
#include <string>
#include <text/json-util.hxx>

#include "guard-fakes.hxx"

using guard_test::GuardBoot;
using guard_test::QuietCameraActions;
using guard_test::RecordingNotifications;
using guard_test::RosterIdentity;
using guard_test::scalar;

namespace
{

GuardBoot& boot()
{
  static GuardBoot shared("guard-episode-test");
  return shared;
}

struct VisitInput
{
  std::string eventId;
  int64_t cameraId{0};
  int64_t trackId{0};
  int64_t personId{0};
  std::string rule;
  std::string severity;
  std::string zoneKind;
};

Json::Value visit(const VisitInput& input)
{
  constexpr int64_t kPublishedAt = 1700000000000;
  Json::Value event(Json::objectValue);
  event["schemaVersion"] = 3;
  event["eventId"] = input.eventId;
  event["cameraId"] = Json::Int64(input.cameraId);
  event["cameraName"] = "Cam " + std::to_string(input.cameraId);
  event["rule"] = input.rule;
  event["severity"] = input.severity;
  event["escalated"] = false;
  event["night"] = false;
  event["trackId"] = Json::Int64(input.trackId);
  event["publishedAt"] = Json::Int64(kPublishedAt);
  Json::Value object(Json::objectValue);
  object["class"] = "person";
  object["confidence"] = 0.9;
  if (input.personId > 0) {
    object["personId"] = Json::Int64(input.personId);
    object["identity"] = "unknown";
  }
  object["identityState"] = "unrecognized";
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
  object["observationId"] = input.eventId;
  object["bbox"] = Json::Value(Json::objectValue);
  Json::Value objects(Json::arrayValue);
  objects.append(object);
  event["objects"] = objects;
  return event;
}

GuardService::Config calmConfig()
{
  GuardService::Config config;
  config.enabled = true;
  config.defaultMode = GuardMode::Home;
  config.greetEnabled = false;
  config.greetReplyEnabled = false;
  config.announceLevel = 9;
  config.alarmLevel = 9;
  config.stagingEnabled = false;
  config.actionCooldownS = 0;
  config.maxActionsPerHour = 1000;
  return config;
}

struct Harness
{
  QuietCameraActions camera;
  RosterIdentity identity{{{7, "es"}}};
  RecordingNotifications notifications;
  GuardService::Config config = calmConfig();

  std::unique_ptr<GuardService> service()
  {
    return std::make_unique<GuardService>(
        GuardService::Dependencies{.bus = nullptr,
                                   .identity = &identity,
                                   .notifications = &notifications,
                                   .actions = &camera,
                                   .assessment = nullptr},
        config);
  }
};

GuardFeatureService feature()
{
  return GuardFeatureService({.identity = nullptr,
                              .defaultMode = GuardMode::Home,
                              .siteDefaults = {}});
}

UpdateGuardSiteDto sitePatch(const Json::Value& body)
{
  return UpdateGuardSiteDto::fromJson(body);
}

void resetSite()
{
  Json::Value body(Json::objectValue);
  body["profile"] = "home";
  body["scheduleEnabled"] = false;
  body["open"] = "";
  body["staffed"] = "";
  body["asleep"] = "";
  body["digestHour"] = -1;
  drogon::sync_wait(feature().updateSite(sitePatch(body)));
}

int localHour()
{
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  return local.tm_hour;
}
}

TEST_CASE("the site is the config until the owner edits it, then a patch")
{
  (void)boot();
  GuardFeatureService service = feature();
  const Json::Value initial = drogon::sync_wait(service.site());
  CHECK(initial["profile"].asString() == "home");
  CHECK(initial["digestHour"].asInt() == 21);

  Json::Value body(Json::objectValue);
  body["profile"] = "commercial";
  body["open"] = "00:00-24:00";
  body["scheduleEnabled"] = true;
  const Json::Value updated =
      drogon::sync_wait(service.updateSite(sitePatch(body)));
  CHECK(updated["profile"].asString() == "commercial");
  CHECK(updated["open"].asString() == "00:00-24:00");
  CHECK(updated["closedMode"].asString() == "away");

  Json::Value later(Json::objectValue);
  later["closedMode"] = "armed";
  const Json::Value patched =
      drogon::sync_wait(service.updateSite(sitePatch(later)));
  CHECK(patched["profile"].asString() == "commercial");
  CHECK(patched["closedMode"].asString() == "armed");

  const Json::Value mode = drogon::sync_wait(service.mode());
  CHECK(mode["profile"].asString() == "commercial");
  CHECK(mode["occupancy"].asString() == "open");
  CHECK(mode["publicPresent"].asBool());
  resetSite();
}

TEST_CASE("site patches refuse malformed hours and wrong types")
{
  Json::Value hours(Json::objectValue);
  hours["staffed"] = "mon-fri 8-18";
  CHECK_THROWS(sitePatch(hours));
  Json::Value types(Json::objectValue);
  types["scheduleEnabled"] = "yes";
  CHECK_THROWS(sitePatch(types));
  Json::Value digest(Json::objectValue);
  digest["digestHour"] = 30;
  CHECK_THROWS(sitePatch(digest));
}

TEST_CASE("camera context is stored per camera and listed")
{
  (void)boot();
  GuardFeatureService service = feature();
  Json::Value body(Json::objectValue);
  body["role"] = "kitchen";
  body["outdoor"] = false;
  body["publicArea"] = false;
  body["activeHours"] = "tue-sun 10:00-23:30";
  const Json::Value saved = drogon::sync_wait(service.setCamera(
      {.cameraId = 501, .context = UpdateCameraContextDto::fromJson(body)}));
  CHECK(saved["role"].asString() == "kitchen");
  body["role"] = "office";
  drogon::sync_wait(service.setCamera(
      {.cameraId = 501, .context = UpdateCameraContextDto::fromJson(body)}));
  const Json::Value listed = drogon::sync_wait(service.cameras());
  bool found = false;
  for (const auto& row : listed) {
    if (row["cameraId"].asInt64() == 501) {
      found = true;
      CHECK(row["role"].asString() == "office");
      CHECK(row["activeHours"].asString() == "tue-sun 10:00-23:30");
    }
  }
  CHECK(found);
  Json::Value invalid(Json::objectValue);
  invalid["role"] = "bedroom";
  CHECK_THROWS(UpdateCameraContextDto::fromJson(invalid));
}

TEST_CASE("an episode keeps its story: list, timeline and review")
{
  (void)boot();
  resetSite();
  Harness harness;
  auto service = harness.service();
  for (int step = 1; step <= 3; ++step)
    REQUIRE(drogon::sync_wait(service->handle(
        visit({.eventId = "ep:" + std::to_string(step),
               .cameraId = 510,
               .trackId = 1,
               .personId = 0,
               .rule = "person_in_alert_zone",
               .severity = "critical",
               .zoneKind = "alert"}),
        1)));
  CHECK(harness.notifications.sent().size() == 1);

  GuardFeatureService api = feature();
  const Json::Value page =
      drogon::sync_wait(api.episodes({.limit = 10, .before = 0}));
  Json::Value episode;
  for (const auto& row : page["rows"]) {
    if (row["cameraId"].asInt64() == 510)
      episode = row;
  }
  REQUIRE(episode.isObject());
  CHECK(episode["kind"].asString() == "person");
  CHECK(episode["state"].asString() == "active");
  CHECK(episode["notified"].asBool());
  CHECK(episode["danger"].asString() == "critical");
  CHECK(episode["subject"].asString() == "stranger");
  CHECK(episode["reasons"][0].asString() == "alert_zone");

  const auto detail =
      drogon::sync_wait(api.episode(episode["id"].asInt64()));
  REQUIRE(detail.has_value());
  const Json::Value& timeline = (*detail)["timeline"];
  REQUIRE(timeline.isArray());
  bool notified = false;
  bool collapsed = false;
  for (const auto& entry : timeline) {
    if (entry["type"] == "action" && entry["action"] == "notify")
      notified = true;
    if (entry["type"] == "decision" && entry["count"].asInt() >= 2)
      collapsed = true;
  }
  CHECK(notified);
  CHECK(collapsed);

  const auto reviewed = drogon::sync_wait(api.reviewEpisode(
      {.episodeId = episode["id"].asInt64(), .label = FeedbackLabel::FalseAlarm}));
  REQUIRE(reviewed.has_value());
  CHECK((*reviewed)["reviewLabel"].asString() == "false_alarm");
  CHECK(scalar("SELECT feedback_label FROM guard_decision_journal WHERE "
               "event_id = 'ep:1'") == "false_alarm");
  CHECK_FALSE(drogon::sync_wait(api.reviewEpisode(
                  {.episodeId = 999999, .label = FeedbackLabel::Useful}))
                  .has_value());
}

TEST_CASE("a second medium visit on the same camera joins the first")
{
  (void)boot();
  resetSite();
  Harness harness;
  auto service = harness.service();
  REQUIRE(drogon::sync_wait(service->handle(
      visit({.eventId = "grp:1",
             .cameraId = 520,
             .trackId = 1,
             .personId = 5201,
             .rule = "person_in_monitor_zone",
             .severity = "warning",
             .zoneKind = "monitor"}),
      1)));
  REQUIRE(drogon::sync_wait(service->handle(
      visit({.eventId = "grp:2",
             .cameraId = 520,
             .trackId = 2,
             .personId = 5202,
             .rule = "person_in_monitor_zone",
             .severity = "warning",
             .zoneKind = "monitor"}),
      1)));
  CHECK(harness.notifications.sent().size() == 1);
  CHECK(scalar("SELECT suppression_reason FROM guard_decision_journal WHERE "
               "event_id = 'grp:2'") == "grouped");
  const std::string first = scalar(
      "SELECT encounter_id FROM guard_decision_journal WHERE event_id = "
      "'grp:1'");
  CHECK(scalar("SELECT group_id FROM guard_encounter WHERE person_id = 5202") ==
        first);
}

TEST_CASE("each recipient reads the alert in their own language")
{
  (void)boot();
  resetSite();
  Harness harness;
  RosterIdentity bilingual({{7, "es"}, {8, "en-US"}});
  auto service = std::make_unique<GuardService>(
      GuardService::Dependencies{.bus = nullptr,
                                 .identity = &bilingual,
                                 .notifications = &harness.notifications,
                                 .actions = &harness.camera,
                                 .assessment = nullptr},
      harness.config);
  REQUIRE(drogon::sync_wait(service->handle(
      visit({.eventId = "lang:1",
             .cameraId = 530,
             .trackId = 1,
             .personId = 0,
             .rule = "person_in_alert_zone",
             .severity = "critical",
             .zoneKind = "alert"}),
      1)));
  const auto sent = harness.notifications.sent();
  REQUIRE(sent.size() == 2);
  int english = 0;
  int spanish = 0;
  for (const auto& notification : sent) {
    const Json::Value data = json_util::fromString(notification.data);
    CHECK(notification.recipients == 1);
    if (data["lang"].asString() == "en") {
      ++english;
      CHECK(notification.title == "Unknown person · Cam 530");
      CHECK(notification.commandId.ends_with(":en"));
    }
    else {
      ++spanish;
      CHECK(notification.title == "Persona desconocida · Cam 530");
      CHECK(notification.commandId.ends_with(":es"));
    }
  }
  CHECK(english == 1);
  CHECK(spanish == 1);
}

TEST_CASE("expected activity is summarized once at the digest hour")
{
  (void)boot();
  GuardFeatureService api = feature();
  Json::Value body(Json::objectValue);
  body["profile"] = "commercial";
  body["scheduleEnabled"] = true;
  body["open"] = "00:00-24:00";
  body["digestHour"] = localHour();
  drogon::sync_wait(api.updateSite(sitePatch(body)));
  Json::Value kitchen(Json::objectValue);
  kitchen["role"] = "kitchen";
  drogon::sync_wait(api.setCamera(
      {.cameraId = 540, .context = UpdateCameraContextDto::fromJson(kitchen)}));

  Harness harness;
  harness.config.stagingEnabled = true;
  auto service = harness.service();
  for (int step = 1; step <= 5; ++step)
    REQUIRE(drogon::sync_wait(service->handle(
        visit({.eventId = "dig:" + std::to_string(step),
               .cameraId = 540,
               .trackId = 1,
               .personId = 0,
               .rule = "person_day",
               .severity = "info",
               .zoneKind = {}}),
        1)));
  CHECK(harness.notifications.sent().empty());
  CHECK(scalar("SELECT reasons FROM guard_decision_journal WHERE event_id = "
               "'dig:5'")
            .find("public_hours") != std::string::npos);

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  drogon::sync_wait(service->maybeSendDigests(now));
  const auto sent = harness.notifications.sent();
  REQUIRE(sent.size() == 1);
  CHECK(sent.front().title == "Resumen de vigilancia");
  CHECK(sent.front().body.find("Cam 540 1") != std::string::npos);
  const Json::Value data = json_util::fromString(sent.front().data);
  CHECK(data["kind"].asString() == "guard_digest");
  CHECK(data["urgency"].asString() == "passive");

  drogon::sync_wait(service->maybeSendDigests(now + 60));
  CHECK(harness.notifications.sent().size() == 1);
  resetSite();
}
