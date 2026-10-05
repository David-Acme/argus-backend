#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "guard-fakes.hxx"

#include <doctest/doctest.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/guard/dtos/create-expected-guest-dto.hxx>
#include <feature/guard/dtos/update-guard-mode-dto.hxx>
#include <feature/guard/guard-copy.hxx>
#include <feature/guard/guard-service.hxx>
#include <feature/guard/services/guard-feature-service.hxx>
#include <feature/safety/dtos/set-pin-dto.hxx>
#include <feature/safety/infra/guard-alert-sink.hxx>
#include <feature/safety/infra/notification-actor-notifier.hxx>
#include <feature/safety/services/pin-attempts.hxx>
#include <feature/safety/services/pin-hash.hxx>
#include <feature/safety/services/safety-service.hxx>
#include <text/json-util.hxx>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using guard_test::GuardBoot;
using guard_test::QuietCameraActions;
using guard_test::RosterIdentity;
using guard_test::scalar;

namespace
{

GuardBoot& boot()
{
  static GuardBoot shared("guard-safety-test");
  return shared;
}

struct Delivered
{
  std::string commandId;
  std::string title;
  std::string body;
  Json::Value data;
  std::vector<int64_t> userIds;
};

class AddressedNotifications final : public NotificationClient
{
public:
  AddressedNotifications() : NotificationClient({.target = "127.0.0.1:1", .credential = {}}) {}

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::client::CallerIdentity&) const override
  {
    NotificationCreateResult result;
    std::scoped_lock lock(mutex_);
    if (failures_ > 0) {
      --failures_;
      result.outcome = NotificationRpcOutcome::Unavailable;
      return result;
    }
    sent_.push_back({.commandId = request.command_id(),
                     .title = request.title(),
                     .body = request.body(),
                     .data = json_util::fromString(request.data()),
                     .userIds = {request.user_ids().begin(), request.user_ids().end()}});
    result.outcome = NotificationRpcOutcome::Success;
    result.created = request.user_ids_size();
    return result;
  }

  [[nodiscard]] std::vector<Delivered> sent() const
  {
    std::scoped_lock lock(mutex_);
    return sent_;
  }

  void clear() const
  {
    std::scoped_lock lock(mutex_);
    sent_.clear();
  }

  void failNext(int count) const
  {
    std::scoped_lock lock(mutex_);
    failures_ = count;
  }

private:
  mutable std::mutex mutex_;
  mutable std::vector<Delivered> sent_;
  mutable int failures_{0};
};

GuardService::Config quickRetries()
{
  GuardService::Config config;
  config.retryBaseMs = 1;
  config.retryMaxMs = 1;
  return config;
}

struct SafetyRig
{
  RosterIdentity identity{{{1, "es"}, {2, "es"}, {3, "en"}}};
  AddressedNotifications notifications;
  QuietCameraActions camera;
  GuardService guard{GuardService::Dependencies{.bus = nullptr,
                                                .identity = &identity,
                                                .notifications = &notifications,
                                                .actions = &camera,
                                                .assessment = nullptr,
                                                .directory = {}},
                     quickRetries()};
  GuardAlertSink sink{guard};
  NotificationActorNotifier actor{
      NotificationActorNotifier::Dependencies{.notifications = &notifications, .identity = &identity}};
  int64_t clock{1'700'000'000};
  SafetyService safety{SafetyService::Dependencies{.sink = &sink,
                                                   .actor = &actor,
                                                   .clock = [this] { return clock; }},
                       SafetyService::Config{.pinIterations = 1000,
                                             .panicRepeatWindowS = 60,
                                             .panicLimit = 3,
                                             .panicLimitWindowS = 3600,
                                             .retryBaseS = 2,
                                             .retryMaxS = 8,
                                             .resumeWindowS = 86400,
                                             .sweepIntervalS = 5.0,
                                             .retentionS = 30LL * 86400,
                                             .attempts = {.maxFailures = 3, .windowSeconds = 900}}};
  GuardFeatureService feature{GuardFeatureDependencies{.identity = &identity, .disarm = &safety}};

  SafetyRig()
  {
    scalar("DELETE FROM guard_user_pin");
    scalar("DELETE FROM guard_safety_setting");
    scalar("DELETE FROM guard_safety_alert");
    scalar("UPDATE guard_environment SET mode = 'away'");
  }

  [[nodiscard]] std::vector<Delivered> ofKind(const std::string& kind) const
  {
    std::vector<Delivered> found;
    for (const auto& row : notifications.sent())
      if (row.data["kind"].asString() == kind)
        found.push_back(row);
    return found;
  }

  Json::Value setMode(const std::string& mode, std::optional<std::string> pin, int64_t userId,
                      std::optional<int64_t> environmentId = std::nullopt)
  {
    Json::Value body(Json::objectValue);
    body["mode"] = mode;
    if (pin)
      body["pin"] = *pin;
    if (environmentId)
      body["environmentId"] = static_cast<Json::Int64>(*environmentId);
    const UpdateGuardModeDto dto = UpdateGuardModeDto::fromJson(body);
    const GuardModeChange change{.body = dto, .userId = userId, .userName = "Laura"};
    return drogon::sync_wait(feature.setMode(change));
  }

  void waitForKind(const std::string& kind, size_t count) const
  {
    for (int attempt = 0; attempt < 300 && ofKind(kind).size() < count; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  void armPins()
  {
    drogon::sync_wait(safety.toggle({.duressEnabled = true, .actorUserId = 1}));
    static_cast<void>(drogon::sync_wait(safety.setPin({.userId = 2,
                                                       .userName = "Laura",
                                                       .disarmPin = "1111",
                                                       .duressPin = "2222",
                                                       .currentPin = std::nullopt})));
  }
};

bool contains(const std::vector<int64_t>& ids, int64_t id)
{
  return std::ranges::find(ids, id) != ids.end();
}

template <typename Call>
int refusalOf(Call&& call)
{
  try {
    call();
  }
  catch (const ResponseException& error) {
    return error.statusCode();
  }
  return 0;
}

}

TEST_CASE("PIN hashes are salted, slow, verifiable and never the PIN")
{
  const std::string one = pin_hash::make({.pin = "4821", .iterations = 1000});
  const std::string two = pin_hash::make({.pin = "4821", .iterations = 1000});
  CHECK(one != two);
  CHECK(one.find("4821") == std::string::npos);
  CHECK(one.starts_with("pbkdf2-sha256$100000$"));
  CHECK(pin_hash::verify({.pin = "4821", .stored = one}));
  CHECK_FALSE(pin_hash::verify({.pin = "4822", .stored = one}));
  CHECK_FALSE(pin_hash::verify({.pin = "4821", .stored = "pbkdf2-sha256$10$aa$bb"}));
  CHECK_FALSE(pin_hash::verify({.pin = "4821", .stored = "garbage"}));
  CHECK(pin_hash::make({.pin = "1", .iterations = 1}).starts_with("pbkdf2-sha256$100000$"));
  CHECK_FALSE(pin_hash::verify(
      {.pin = "4821", .stored = "pbkdf2-sha256$1000$00112233445566778899aabbccddeeff$"
                                "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"}));
  CHECK(pin_hash::wellFormedPin("0000"));
  CHECK_FALSE(pin_hash::wellFormedPin("12a4"));
  CHECK_FALSE(pin_hash::wellFormedPin("123"));
}

TEST_CASE("trivial PINs are recognised")
{
  for (const char* pin : {"0000", "1111", "1234", "4321", "9012", "123456", "1212", "123123"})
    CHECK(pin_hash::trivialPin(pin));
  for (const char* pin : {"4721", "9386", "130572", "2580"})
    CHECK_FALSE(pin_hash::trivialPin(pin));
}

TEST_CASE("wrong PINs lock the user for the window, then let go")
{
  PinAttempts attempts({.maxFailures = 2, .windowSeconds = 100});
  REQUIRE(attempts.reserve({.userId = 7, .now = 10}).admitted);
  attempts.settle({.userId = 7, .success = false});
  CHECK_FALSE(attempts.locked({.userId = 7, .now = 11}));
  REQUIRE(attempts.reserve({.userId = 7, .now = 20}).admitted);
  attempts.settle({.userId = 7, .success = false});
  CHECK(attempts.locked({.userId = 7, .now = 21}));
  CHECK_FALSE(attempts.locked({.userId = 8, .now = 21}));
  CHECK_FALSE(attempts.locked({.userId = 7, .now = 111}));
  REQUIRE(attempts.reserve({.userId = 7, .now = 112}).admitted);
  attempts.settle({.userId = 7, .success = true});
  CHECK_FALSE(attempts.locked({.userId = 7, .now = 113}));
}

TEST_CASE("an attempt is counted before hashing and one runs per user")
{
  PinAttempts attempts({.maxFailures = 2, .windowSeconds = 100});
  const PinReservation first = attempts.reserve({.userId = 7, .now = 10});
  CHECK(first.admitted);
  CHECK_FALSE(first.locked);
  CHECK_FALSE(attempts.reserve({.userId = 7, .now = 10}).admitted);
  CHECK(attempts.reserve({.userId = 8, .now = 10}).admitted);
  attempts.settle({.userId = 7, .success = false});
  REQUIRE(attempts.reserve({.userId = 7, .now = 11}).admitted);
  CHECK(attempts.locked({.userId = 7, .now = 11}));
  attempts.settle({.userId = 7, .success = false});
  const PinReservation locked = attempts.reserve({.userId = 7, .now = 12});
  CHECK(locked.admitted);
  CHECK(locked.locked);
  attempts.settle({.userId = 7, .success = false});
}

TEST_CASE("the two PINs must be digits and different")
{
  Json::Value body(Json::objectValue);
  body["disarmPin"] = "4721";
  body["duressPin"] = "4721";
  CHECK_THROWS_AS(SetPinDto::fromJson(body), ValidationException);
  body["duressPin"] = "12x4";
  CHECK_THROWS_AS(SetPinDto::fromJson(body), ValidationException);
  body["duressPin"] = "1234";
  CHECK_THROWS_AS(SetPinDto::fromJson(body), ValidationException);
  body["duressPin"] = "9386";
  CHECK(SetPinDto::fromJson(body).duressPin == "9386");
  CHECK_FALSE(SetPinDto::fromJson(body).currentPin.has_value());
  body["currentPin"] = "55";
  CHECK_THROWS_AS(SetPinDto::fromJson(body), ValidationException);
  body["currentPin"] = "5501";
  CHECK(SetPinDto::fromJson(body).currentPin == std::optional<std::string>("5501"));
  body["disarmPin"] = "0000";
  CHECK_THROWS_AS(SetPinDto::fromJson(body), ValidationException);
  Json::Value mode(Json::objectValue);
  mode["mode"] = "home";
  mode["pin"] = "12";
  CHECK_THROWS_AS(UpdateGuardModeDto::fromJson(mode), ValidationException);
}

TEST_CASE("without duress PINs, disarming is exactly as before")
{
  boot();
  SafetyRig rig;
  const Json::Value environments = rig.setMode("home", std::nullopt, 2);
  CHECK(environments[0]["mode"].asString() == "home");
  CHECK(rig.notifications.sent().empty());
  CHECK_THROWS_AS(drogon::sync_wait(rig.safety.setPin({.userId = 2, .userName = "Laura", .disarmPin = "1111", .duressPin = "2222", .currentPin = std::nullopt})),
                  ResponseException);
}

TEST_CASE("a duress PIN disarms like the real one and silently alerts everyone else")
{
  boot();
  SafetyRig rig;
  drogon::sync_wait(rig.safety.toggle({.duressEnabled = true, .actorUserId = 1}));
  const SafetyStatus status =
      drogon::sync_wait(rig.safety.setPin({.userId = 2, .userName = "Laura", .disarmPin = "1111", .duressPin = "2222", .currentPin = std::nullopt}));
  CHECK(status.hasPin);
  CHECK(scalar("SELECT COUNT(*) FROM guard_user_pin WHERE disarm_hash LIKE '%1111%' OR "
               "duress_hash LIKE '%2222%'") == "0");

  CHECK_THROWS_AS(rig.setMode("home", std::nullopt, 2), ResponseException);
  CHECK(scalar("SELECT mode FROM guard_environment WHERE is_default = 1") == "away");

  const Json::Value normal = rig.setMode("home", std::string("1111"), 2);
  CHECK(rig.notifications.sent().empty());
  scalar("UPDATE guard_environment SET mode = 'away'");

  const Json::Value forced = rig.setMode("home", std::string("2222"), 2);
  CHECK(json_util::toString(forced[0]["mode"]) == json_util::toString(normal[0]["mode"]));
  CHECK(forced[0]["mode"].asString() == "home");
  rig.waitForKind("guard_duress", 1);
  for (int attempt = 0; attempt < 300 && scalar("SELECT COUNT(*) FROM guard_safety_alert WHERE "
                                                "kind = 'duress' AND notified_at > 0") != "1";
       ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto alerts = rig.ofKind("guard_duress");
  REQUIRE_FALSE(alerts.empty());
  std::vector<int64_t> recipients;
  for (const auto& alert : alerts) {
    recipients.insert(recipients.end(), alert.userIds.begin(), alert.userIds.end());
    CHECK(alert.data["urgency"].asString() == "critical");
    CHECK(alert.data["actorUserId"].asInt64() == 2);
    CHECK(alert.data["cameraId"].asInt64() == 0);
    CHECK(alert.data["threadKey"].asString().starts_with("guard:duress:"));
  }
  CHECK_FALSE(contains(recipients, 2));
  CHECK(contains(recipients, 1));
  CHECK(contains(recipients, 3));
  CHECK(rig.ofKind("guard_panic_sent").empty());
  CHECK(scalar("SELECT COUNT(*) FROM guard_safety_alert WHERE kind = 'duress' AND user_id = 2 "
               "AND notified_at > 0") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_incident WHERE rule IN ('duress', 'panic')") == "0");
}

TEST_CASE("wrong PINs are refused, counted and locked out")
{
  boot();
  SafetyRig rig;
  drogon::sync_wait(rig.safety.toggle({.duressEnabled = true, .actorUserId = 1}));
  static_cast<void>(
      drogon::sync_wait(rig.safety.setPin({.userId = 2, .userName = "Laura", .disarmPin = "1111", .duressPin = "2222", .currentPin = std::nullopt})));
  for (int attempt = 0; attempt < 3; ++attempt)
    CHECK_THROWS_AS(rig.setMode("home", std::string("9999"), 2), ResponseException);
  CHECK_THROWS_AS(rig.setMode("home", std::string("1111"), 2), ResponseException);
  CHECK(scalar("SELECT mode FROM guard_environment WHERE is_default = 1") == "away");
  rig.clock += 901;
  CHECK(rig.setMode("home", std::string("1111"), 2)[0]["mode"].asString() == "home");
  CHECK(rig.setMode("away", std::nullopt, 2)[0]["mode"].asString() == "away");
}

TEST_CASE("switching duress off forgets every stored PIN")
{
  boot();
  SafetyRig rig;
  drogon::sync_wait(rig.safety.toggle({.duressEnabled = true, .actorUserId = 1}));
  static_cast<void>(
      drogon::sync_wait(rig.safety.setPin({.userId = 2, .userName = "Laura", .disarmPin = "1111", .duressPin = "2222", .currentPin = std::nullopt})));
  drogon::sync_wait(rig.safety.toggle({.duressEnabled = false, .actorUserId = 1}));
  CHECK(scalar("SELECT COUNT(*) FROM guard_user_pin") == "0");
  CHECK_FALSE(drogon::sync_wait(rig.safety.status(2)).hasPin);
  CHECK(rig.setMode("home", std::nullopt, 2)[0]["mode"].asString() == "home");
}

TEST_CASE("panic alerts the household critically, never rings the actor, and confirms silently")
{
  boot();
  SafetyRig rig;
  const PanicResult result = drogon::sync_wait(
      rig.safety.panic({.userId = 3, .userName = "Marta", .environmentId = std::nullopt}));
  CHECK(result.sent);
  CHECK_FALSE(result.repeated);
  const auto alerts = rig.ofKind("guard_panic");
  REQUIRE_FALSE(alerts.empty());
  std::vector<int64_t> recipients;
  for (const auto& alert : alerts) {
    recipients.insert(recipients.end(), alert.userIds.begin(), alert.userIds.end());
    CHECK(alert.data["urgency"].asString() == "critical");
    CHECK(alert.data["actorName"].asString() == "Marta");
    CHECK(alert.data["episodeId"].asInt64() == result.alertId);
    CHECK(alert.title.find("Marta") != std::string::npos);
  }
  CHECK_FALSE(contains(recipients, 3));
  CHECK(contains(recipients, 1));
  const auto confirmations = rig.ofKind("guard_panic_sent");
  REQUIRE(confirmations.size() == 1);
  CHECK(confirmations[0].userIds == std::vector<int64_t>{3});
  CHECK(confirmations[0].data["urgency"].asString() == "passive");
  CHECK(confirmations[0].data["silent"].asBool());
  CHECK(confirmations[0].title == "Alert sent");

  const PanicResult again = drogon::sync_wait(
      rig.safety.panic({.userId = 3, .userName = "Marta", .environmentId = std::nullopt}));
  CHECK(again.repeated);
  CHECK(again.alertId == result.alertId);
  CHECK(rig.ofKind("guard_panic").size() == alerts.size());
}

TEST_CASE("a panic the notification service missed is swept with backoff until delivered")
{
  boot();
  SafetyRig rig;
  rig.notifications.failNext(1);
  const PanicResult result = drogon::sync_wait(
      rig.safety.panic({.userId = 1, .userName = "Ana", .environmentId = std::nullopt}));
  CHECK_FALSE(result.sent);
  const std::string notified =
      "SELECT notified_at > 0 FROM guard_safety_alert WHERE id = " + std::to_string(result.alertId);
  CHECK(drogon::sync_wait(rig.safety.sweepPending()) == 0);
  rig.clock += 1;
  CHECK(drogon::sync_wait(rig.safety.sweepPending()) == 0);
  CHECK(rig.ofKind("guard_panic").empty());
  rig.clock += 1;
  CHECK(drogon::sync_wait(rig.safety.sweepPending()) == 1);
  CHECK_FALSE(rig.ofKind("guard_panic").empty());
  CHECK(scalar(notified) == "1");
  CHECK(drogon::sync_wait(rig.safety.sweepPending()) == 0);

  const PanicResult again = drogon::sync_wait(
      rig.safety.panic({.userId = 1, .userName = "Ana", .environmentId = std::nullopt}));
  CHECK(again.repeated);
  CHECK(again.sent);
}

TEST_CASE("panic presses are limited per user and hour")
{
  boot();
  SafetyRig rig;
  std::vector<int64_t> alerts;
  for (int press = 0; press < 3; ++press) {
    const PanicResult result = drogon::sync_wait(
        rig.safety.panic({.userId = 3, .userName = "Marta", .environmentId = std::nullopt}));
    CHECK_FALSE(result.repeated);
    alerts.push_back(result.alertId);
    rig.clock += 61;
  }
  const PanicResult limited = drogon::sync_wait(
      rig.safety.panic({.userId = 3, .userName = "Marta", .environmentId = std::nullopt}));
  CHECK(limited.repeated);
  CHECK(limited.alertId == alerts.back());
  rig.clock += 3600;
  CHECK_FALSE(drogon::sync_wait(
                  rig.safety.panic({.userId = 3, .userName = "Marta", .environmentId = std::nullopt}))
                  .repeated);
}

TEST_CASE("changing or removing the PINs needs the current one, and the duress PIN alerts")
{
  boot();
  SafetyRig rig;
  rig.armPins();
  CHECK(refusalOf([&] {
          static_cast<void>(drogon::sync_wait(rig.safety.removePin(
              {.userId = 2, .userName = "Laura", .currentPin = std::nullopt})));
        }) == 403);
  CHECK(refusalOf([&] {
          static_cast<void>(drogon::sync_wait(rig.safety.setPin({.userId = 2,
                                                                 .userName = "Laura",
                                                                 .disarmPin = "4721",
                                                                 .duressPin = "9386",
                                                                 .currentPin = "5555"})));
        }) == 403);
  CHECK(scalar("SELECT COUNT(*) FROM guard_user_pin WHERE user_id = 2") == "1");
  CHECK(drogon::sync_wait(rig.safety.setPin({.userId = 2,
                                             .userName = "Laura",
                                             .disarmPin = "4721",
                                             .duressPin = "9386",
                                             .currentPin = "1111"}))
            .hasPin);
  CHECK(rig.notifications.sent().empty());
  const SafetyStatus removed = drogon::sync_wait(
      rig.safety.removePin({.userId = 2, .userName = "Laura", .currentPin = "9386"}));
  CHECK_FALSE(removed.hasPin);
  CHECK(scalar("SELECT COUNT(*) FROM guard_user_pin WHERE user_id = 2") == "0");
  rig.waitForKind("guard_duress", 1);
  CHECK_FALSE(rig.ofKind("guard_duress").empty());
}

TEST_CASE("a locked-out user still raises the silent alert with the duress PIN")
{
  boot();
  SafetyRig rig;
  rig.armPins();
  for (int attempt = 0; attempt < 3; ++attempt)
    CHECK(refusalOf([&] { rig.setMode("home", std::string("9999"), 2); }) == 403);
  CHECK(refusalOf([&] { rig.setMode("home", std::string("1111"), 2); }) == 429);
  CHECK(rig.notifications.sent().empty());
  CHECK(refusalOf([&] { rig.setMode("home", std::string("2222"), 2); }) == 429);
  rig.waitForKind("guard_duress", 1);
  CHECK_FALSE(rig.ofKind("guard_duress").empty());
  CHECK(scalar("SELECT mode FROM guard_environment WHERE is_default = 1") == "away");
}

TEST_CASE("lowering any mode asks for the PIN, and a duress PIN alerts before the environment check")
{
  boot();
  SafetyRig rig;
  rig.armPins();
  scalar("UPDATE guard_environment SET mode = 'armed'");
  CHECK(refusalOf([&] { rig.setMode("away", std::nullopt, 2); }) == 403);
  CHECK(rig.setMode("away", std::string("1111"), 2)[0]["mode"].asString() == "away");
  CHECK(rig.setMode("armed", std::nullopt, 2)[0]["mode"].asString() == "armed");
  CHECK(refusalOf([&] { rig.setMode("home", std::string("2222"), 2, 99999); }) == 404);
  rig.waitForKind("guard_duress", 1);
  CHECK_FALSE(rig.ofKind("guard_duress").empty());
}

TEST_CASE("a disabled account loses its PINs and old alerts are purged")
{
  boot();
  SafetyRig rig;
  rig.armPins();
  drogon::sync_wait(rig.safety.forgetUser(2));
  CHECK(scalar("SELECT COUNT(*) FROM guard_user_pin WHERE user_id = 2") == "0");
  scalar("INSERT INTO guard_safety_alert (kind, user_id, environment_id, created_at, "
         "notified_at) VALUES ('panic', 3, 0, " +
         std::to_string(rig.clock - 40LL * 86400) + ", 1)");
  scalar("INSERT INTO guard_safety_alert (kind, user_id, environment_id, created_at, "
         "notified_at) VALUES ('panic', 3, 0, " +
         std::to_string(rig.clock - 86400) + ", 1)");
  CHECK(drogon::sync_wait(rig.safety.purgeExpired()) == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_safety_alert") == "1");
}

TEST_CASE("safety notices render in both languages")
{
  GuardNotice notice{.kind = NoticeKind::Panic,
                     .subject = NoticeSubject::Stranger,
                     .people = 0,
                     .cameraId = 0,
                     .cameraName = {},
                     .environmentName = {},
                     .role = CameraRole::Other,
                     .outdoor = false,
                     .zoneName = {},
                     .reasons = {},
                     .dwellS = 0,
                     .danger = GuardDanger::Critical,
                     .action = NoticeAction::Watching,
                     .tamperStatus = {},
                     .held = {},
                     .routine = {},
                     .notified = 0,
                     .afterQuiet = false,
                     .actorName = "Laura",
                     .visitor = {}};
  CHECK(guard_copy::render(notice, "es").title == "Botón de pánico · Laura");
  CHECK(guard_copy::urgency(notice) == "critical");
  notice.kind = NoticeKind::Duress;
  notice.environmentName = "Casa";
  CHECK(guard_copy::render(notice, "en").title == "Silent alert · Laura (Casa)");
  CHECK(noticeKindToString(notice.kind) == "guard_duress");
}

TEST_CASE("expected visits are bounded, scoped for residents and hosted by the caller")
{
  boot();
  SafetyRig rig;
  const auto create = [&rig](const Json::Value& body, UserRole role) {
    const CreateExpectedGuestDto dto = CreateExpectedGuestDto::fromJson(body);
    return drogon::sync_wait(
        rig.feature.createGuest({.body = dto, .callerId = 2, .callerRole = role}));
  };
  Json::Value open(Json::objectValue);
  open["description"] = "anyone";
  CHECK(refusalOf([&] { create(open, UserRole::Resident); }) == 422);
  Json::Value cameraOnly = open;
  cameraOnly["cameraId"] = 4;
  CHECK(refusalOf([&] { create(cameraOnly, UserRole::Resident); }) == 422);
  Json::Value person = open;
  person["personId"] = 9;
  const int64_t personPass = create(person, UserRole::Resident);
  CHECK(scalar("SELECT host_user_id FROM guard_expected_guest WHERE id = " +
               std::to_string(personPass)) == "2");
  Json::Value otherHost = person;
  otherHost["hostUserId"] = 3;
  CHECK(refusalOf([&] { create(otherHost, UserRole::Resident); }) == 403);
  CHECK(create(otherHost, UserRole::Owner) > 0);
  CHECK(create(open, UserRole::Owner) > 0);
  Json::Value endless = person;
  endless["validFrom"] = static_cast<Json::Int64>(1'700'000'000);
  endless["validUntil"] = static_cast<Json::Int64>(4'102'444'800);
  CHECK(refusalOf([&] { create(endless, UserRole::Owner); }) == 422);
  endless["validUntil"] = static_cast<Json::Int64>(1'700'000'000 + 24 * 3600);
  CHECK(create(endless, UserRole::Owner) > 0);
}
