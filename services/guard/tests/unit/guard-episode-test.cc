#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <ctime>
#include <doctest/doctest.h>
#include <feature/guard/dtos/update-camera-context-dto.hxx>
#include <errors/response-exception.hxx>
#include <feature/guard/dtos/create-environment-dto.hxx>
#include <feature/guard/dtos/update-environment-dto.hxx>
#include <feature/guard/dtos/update-guard-mode-dto.hxx>
#include <feature/guard/guard-service.hxx>
#include <feature/guard/services/guard-feature-service.hxx>
#include <memory>
#include <optional>
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
  config.staleObservationS = 0;
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

class FixedDirectory final : public ResponseDirectory
{
public:
  explicit FixedDirectory(std::vector<ResponseUser> users) : users_(std::move(users)) {}

  [[nodiscard]] std::optional<std::vector<ResponseUser>> users() const override
  {
    return users_;
  }

private:
  std::vector<ResponseUser> users_;
};

struct Harness
{
  QuietCameraActions camera;
  RosterIdentity identity{{{7, "es"}}};
  RecordingNotifications notifications;
  GuardService::Config config = calmConfig();
  std::shared_ptr<const ResponseDirectory> directory;

  std::unique_ptr<GuardService> service()
  {
    return std::make_unique<GuardService>(
        GuardService::Dependencies{.bus = nullptr,
                                   .identity = &identity,
                                   .notifications = &notifications,
                                   .actions = &camera,
                                   .assessment = nullptr,
                                   .directory = directory},
        config);
  }
};

GuardFeatureService feature()
{
  return GuardFeatureService({.identity = nullptr});
}

UpdateEnvironmentDto patchOf(const Json::Value& body)
{
  return UpdateEnvironmentDto::fromJson(body);
}

int64_t homeId()
{
  return std::stoll(scalar("SELECT id FROM guard_environment WHERE is_default = 1"));
}

Json::Value findEnvironment(const Json::Value& list, int64_t id)
{
  for (const auto& row : list) {
    if (row["id"].asInt64() == id)
      return row;
  }
  return {};
}

void resetSite()
{
  Json::Value body(Json::objectValue);
  body["kind"] = "home";
  body["scheduleEnabled"] = false;
  body["open"] = "";
  body["staffed"] = "";
  body["asleep"] = "";
  body["digestHour"] = -1;
  const UpdateEnvironmentDto patch = patchOf(body);
  drogon::sync_wait(feature().updateEnvironment({.id = homeId(), .patch = patch}));
}

Json::Value applyMode(const GuardFeatureService& service, const Json::Value& body)
{
  const UpdateGuardModeDto dto = UpdateGuardModeDto::fromJson(body);
  const GuardModeChange change{.body = dto, .userId = 2, .userName = "Test"};
  return drogon::sync_wait(service.setMode(change));
}

Json::Value modeBody(const std::string& mode, int64_t environmentId)
{
  Json::Value body(Json::objectValue);
  body["mode"] = mode;
  if (environmentId > 0)
    body["environmentId"] = Json::Int64(environmentId);
  return body;
}

int localHour()
{
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  return local.tm_hour;
}
}

template <typename T>
T present(const std::optional<T>& value)
{
  REQUIRE(value.has_value());
  return value.value_or(T{});
}

TEST_CASE("environments: the seeded default, a second place, patches and modes")
{
  (void)boot();
  GuardFeatureService service = feature();
  const Json::Value initial = drogon::sync_wait(service.environments());
  REQUIRE(initial.size() == 1);
  CHECK(initial[0]["isDefault"].asBool());
  CHECK(initial[0]["kind"].asString() == "home");
  CHECK(initial[0]["name"].asString() == "Casa");
  CHECK(initial[0]["mode"].asString() == "home");

  Json::Value body(Json::objectValue);
  body["name"] = "  Trattoria  ";
  body["kind"] = "restaurant";
  body["scheduleEnabled"] = true;
  body["open"] = "00:00-24:00";
  const Json::Value created = drogon::sync_wait(
      service.createEnvironment(CreateEnvironmentDto::fromJson(body)));
  const int64_t restaurant = created["id"].asInt64();
  CHECK(created["name"].asString() == "Trattoria");
  CHECK(created["occupancy"].asString() == "open");
  CHECK(created["publicPresent"].asBool());
  CHECK_FALSE(created["isDefault"].asBool());

  Json::Value clash(Json::objectValue);
  clash["name"] = "trattoria";
  clash["kind"] = "office";
  CHECK_THROWS_AS(drogon::sync_wait(service.createEnvironment(
                      CreateEnvironmentDto::fromJson(clash))),
                  ResponseException);

  Json::Value later(Json::objectValue);
  later["closedMode"] = "armed";
  later["quietPolicy"] = "custom";
  later["quietStartHour"] = 1;
  later["quietEndHour"] = 6;
  const UpdateEnvironmentDto laterPatch = patchOf(later);
  const Json::Value patched = drogon::sync_wait(
      service.updateEnvironment({.id = restaurant, .patch = laterPatch}));
  CHECK(patched["kind"].asString() == "restaurant");
  CHECK(patched["open"].asString() == "00:00-24:00");
  CHECK(patched["closedMode"].asString() == "armed");
  CHECK(patched["quietPolicy"].asString() == "custom");
  CHECK(patched["quietEndHour"].asInt() == 6);
  CHECK_THROWS_AS(drogon::sync_wait(service.updateEnvironment(
                      {.id = 999999, .patch = laterPatch})),
                  ResponseException);

  const Json::Value one = applyMode(service, modeBody("armed", restaurant));
  CHECK(findEnvironment(one, restaurant)["mode"].asString() == "armed");
  CHECK(findEnvironment(one, restaurant)["effectiveMode"].asString() == "armed");
  CHECK(findEnvironment(one, homeId())["mode"].asString() == "home");
  CHECK_THROWS_AS(applyMode(service, modeBody("away", 999999)),
                  ResponseException);
  const Json::Value all = applyMode(service, modeBody("night", 0));
  for (const auto& row : all)
    CHECK(row["mode"].asString() == "night");
  applyMode(service, modeBody("home", 0));

  Json::Value kitchen(Json::objectValue);
  kitchen["role"] = "kitchen";
  kitchen["environmentId"] = Json::Int64(restaurant);
  drogon::sync_wait(service.setCamera(
      {.cameraId = 601, .context = UpdateCameraContextDto::fromJson(kitchen)}));
  CHECK(findEnvironment(drogon::sync_wait(service.environments()), restaurant)
            ["cameraIds"][0]
                .asInt64() == 601);
  CHECK_THROWS_AS(drogon::sync_wait(service.removeEnvironment(homeId())),
                  ResponseException);
  const Json::Value remaining =
      drogon::sync_wait(service.removeEnvironment(restaurant));
  CHECK(remaining.size() == 1);
  CHECK(scalar("SELECT environment_id FROM guard_camera_context WHERE "
               "camera_id = 601") == std::to_string(homeId()));
  CHECK_THROWS_AS(drogon::sync_wait(service.removeEnvironment(restaurant)),
                  ResponseException);
}

TEST_CASE("environment bodies refuse malformed hours, names and wrong types")
{
  Json::Value hours(Json::objectValue);
  hours["staffed"] = "mon-fri 8-18";
  CHECK_THROWS(patchOf(hours));
  Json::Value types(Json::objectValue);
  types["scheduleEnabled"] = "yes";
  CHECK_THROWS(patchOf(types));
  Json::Value digest(Json::objectValue);
  digest["digestHour"] = 30;
  CHECK_THROWS(patchOf(digest));
  Json::Value quiet(Json::objectValue);
  quiet["quietPolicy"] = "sometimes";
  CHECK_THROWS(patchOf(quiet));
  Json::Value blank(Json::objectValue);
  blank["name"] = "   ";
  CHECK_THROWS(patchOf(blank));
  Json::Value nameless(Json::objectValue);
  nameless["kind"] = "office";
  CHECK_THROWS(CreateEnvironmentDto::fromJson(nameless));
  Json::Value kindless(Json::objectValue);
  kindless["name"] = "Bodega";
  CHECK_THROWS(CreateEnvironmentDto::fromJson(kindless));
  Json::Value badKind(Json::objectValue);
  badKind["name"] = "Bodega";
  badKind["kind"] = "castle";
  CHECK_THROWS(CreateEnvironmentDto::fromJson(badKind));
  Json::Value negative = modeBody("away", 0);
  negative["environmentId"] = -3;
  CHECK_THROWS(UpdateGuardModeDto::fromJson(negative));
  Json::Value textual = modeBody("away", 0);
  textual["environmentId"] = "2";
  CHECK_THROWS(UpdateGuardModeDto::fromJson(textual));
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
  Json::Value elsewhere(Json::objectValue);
  elsewhere["role"] = "office";
  elsewhere["environmentId"] = Json::Int64(999999);
  CHECK_THROWS_AS(drogon::sync_wait(service.setCamera(
                      {.cameraId = 501,
                       .context = UpdateCameraContextDto::fromJson(elsewhere)})),
                  ResponseException);
  for (const auto& row : drogon::sync_wait(service.cameras())) {
    if (row["cameraId"].asInt64() == 501)
      CHECK(row["environmentId"].asInt64() == homeId());
  }
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
  const Json::Value page = drogon::sync_wait(
      api.episodes({.limit = 10, .before = 0, .environmentId = homeId()}));
  CHECK(drogon::sync_wait(
            api.episodes({.limit = 10, .before = 0, .environmentId = 999999}))
            ["rows"]
                .empty());
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
  CHECK(episode["environmentId"].asInt64() == homeId());

  const auto detail =
      drogon::sync_wait(api.episode(episode["id"].asInt64()));
  if (!detail) {
    FAIL("expected a value in detail");
    return;
  }
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
  if (!reviewed) {
    FAIL("expected a value in reviewed");
    return;
  }
  CHECK((*reviewed)["reviewLabel"].asString() == "false_alarm");
  CHECK(scalar("SELECT feedback_label FROM guard_decision_journal WHERE "
               "event_id = 'ep:1'") == "false_alarm");
  CHECK_FALSE(drogon::sync_wait(api.reviewEpisode(
                  {.episodeId = 999999, .label = FeedbackLabel::Useful}))
                  .has_value());

  const int64_t episodeId = episode["id"].asInt64();
  const auto kept = present(drogon::sync_wait(api.retainEpisode({.episodeId = episodeId, .retain = true})));
  CHECK(kept["retainUntil"].asInt64() >
        static_cast<int64_t>(std::time(nullptr)) + 100LL * 86400);
  const GuardRepository repository;
  const int64_t future = static_cast<int64_t>(std::time(nullptr)) + 10;
  drogon::sync_wait(repository.purgeDecisions(future));
  drogon::sync_wait(repository.purgeHistory({.historyBefore = future, .inboxBefore = 0}));
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = 'ep:1'") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_encounter WHERE id = " +
               std::to_string(episodeId)) == "1");
  const auto released =
      present(drogon::sync_wait(api.retainEpisode({.episodeId = episodeId, .retain = false})));
  CHECK(released["retainUntil"].asInt64() == 0);
  drogon::sync_wait(repository.purgeDecisions(future));
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = 'ep:1'") == "0");
  CHECK_FALSE(drogon::sync_wait(api.retainEpisode({.episodeId = 999999, .retain = true}))
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
  body["kind"] = "commercial";
  body["scheduleEnabled"] = true;
  body["open"] = "00:00-24:00";
  body["digestHour"] = localHour();
  const UpdateEnvironmentDto patch = patchOf(body);
  drogon::sync_wait(api.updateEnvironment({.id = homeId(), .patch = patch}));
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

  const auto now = static_cast<int64_t>(std::time(nullptr));
  drogon::sync_wait(service->maybeSendDigests(now));
  const auto sent = harness.notifications.sent();
  REQUIRE(sent.size() == 1);
  CHECK(sent.front().title == "Resumen de vigilancia");
  CHECK(sent.front().body.find("Cam 540 1") != std::string::npos);
  const Json::Value data = json_util::fromString(sent.front().data);
  CHECK(data["kind"].asString() == "guard_digest");
  CHECK(data["urgency"].asString() == "passive");
  CHECK(data["environmentId"].asInt64() == homeId());
  CHECK(data["threadKey"].asString().starts_with(
      "guard:digest:" + std::to_string(homeId()) + ":"));

  drogon::sync_wait(service->maybeSendDigests(now + 60));
  CHECK(harness.notifications.sent().size() == 1);
  resetSite();
}

TEST_CASE("a watchlist person alerts at once and the notice says who it is")
{
  (void)boot();
  resetSite();
  Harness harness;
  harness.directory = std::make_shared<FixedDirectory>(std::vector<ResponseUser>{
      {.userId = 7, .role = UserRole::Owner, .active = true, .name = "Ana", .lang = "es"}});
  harness.identity.addPerson({.personId = 4242,
                              .userId = std::nullopt,
                              .name = "Hombre de la moto",
                              .alias = {},
                              .observation = {},
                              .role = {},
                              .tags = {},
                              .trusted = false,
                              .category = "watchlist",
                              .visits = 3,
                              .firstSeenAt = 0,
                              .lastSeenAt = 0,
                              .visitorNumber = 7,
                              .usualWeekdays = {},
                              .usualHour = std::nullopt});
  auto service = harness.service();
  REQUIRE(drogon::sync_wait(service->handle(visit({.eventId = "watch:1",
                                                   .cameraId = 530,
                                                   .trackId = 1,
                                                   .personId = 4242,
                                                   .rule = "person_day",
                                                   .severity = "info",
                                                   .zoneKind = ""}),
                                            1)));
  const auto all = harness.notifications.sent();
  REQUIRE(all.size() == 1);
  const auto& sent = all.front();
  CHECK(sent.title.starts_with("Hombre de la moto · "));
  CHECK(sent.body.find("lista de vigilancia") != std::string::npos);
  const Json::Value data = json_util::fromString(sent.data);
  CHECK(data["danger"].asString() == "high");
  CHECK(data["visitor"]["category"].asString() == "watchlist");
  CHECK(data["reasons"][0].asString() == "watchlist");
}

TEST_CASE("without a directory the roster fallback never names a visitor")
{
  (void)boot();
  resetSite();
  Harness harness;
  harness.identity.addPerson({.personId = 4343,
                              .userId = std::nullopt,
                              .name = "Hombre de la moto",
                              .alias = {},
                              .observation = {},
                              .role = {},
                              .tags = {},
                              .trusted = false,
                              .category = "watchlist",
                              .visits = 3,
                              .firstSeenAt = 0,
                              .lastSeenAt = 0,
                              .visitorNumber = 8,
                              .usualWeekdays = {},
                              .usualHour = std::nullopt});
  auto service = harness.service();
  REQUIRE(drogon::sync_wait(service->handle(visit({.eventId = "watch:roster",
                                                   .cameraId = 531,
                                                   .trackId = 1,
                                                   .personId = 4343,
                                                   .rule = "person_day",
                                                   .severity = "info",
                                                   .zoneKind = ""}),
                                            1)));
  const auto all = harness.notifications.sent();
  REQUIRE(all.size() == 1);
  CHECK(all.front().title.find("Hombre de la moto") == std::string::npos);
  CHECK(all.front().body.find("Hombre de la moto") == std::string::npos);
  CHECK(all.front().data.find("Hombre de la moto") == std::string::npos);
  CHECK_FALSE(json_util::fromString(all.front().data).isMember("visitor"));
}
