#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "guard-fakes.hxx"

#include <doctest/doctest.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
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
#include <map>
#include <memory>
#include <mutex>
#include <string>
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
                     GuardService::Config{}};
  GuardAlertSink sink{guard};
  NotificationActorNotifier actor{
      NotificationActorNotifier::Dependencies{.notifications = &notifications, .identity = &identity}};
  int64_t clock{1'700'000'000};
  SafetyService safety{SafetyService::Dependencies{.sink = &sink,
                                                   .actor = &actor,
                                                   .clock = [this] { return clock; }},
                       SafetyService::Config{.pinIterations = 1000,
                                             .panicRepeatWindowS = 60,
                                             .deliveryAttempts = 2,
                                             .resumeWindowS = 900,
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

  Json::Value setMode(const std::string& mode, std::optional<std::string> pin, int64_t userId)
  {
    Json::Value body(Json::objectValue);
    body["mode"] = mode;
    if (pin)
      body["pin"] = *pin;
    const UpdateGuardModeDto dto = UpdateGuardModeDto::fromJson(body);
    const GuardModeChange change{.body = dto, .userId = userId, .userName = "Laura"};
    return drogon::sync_wait(feature.setMode(change));
  }

  void waitForKind(const std::string& kind, size_t count) const
  {
    for (int attempt = 0; attempt < 300 && ofKind(kind).size() < count; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
};

bool contains(const std::vector<int64_t>& ids, int64_t id)
{
  return std::ranges::find(ids, id) != ids.end();
}

}

TEST_CASE("PIN hashes are salted, slow, verifiable and never the PIN")
{
  const std::string one = pin_hash::make({.pin = "4821", .iterations = 1000});
  const std::string two = pin_hash::make({.pin = "4821", .iterations = 1000});
  CHECK(one != two);
  CHECK(one.find("4821") == std::string::npos);
  CHECK(one.starts_with("pbkdf2-sha256$1000$"));
  CHECK(pin_hash::verify({.pin = "4821", .stored = one}));
  CHECK_FALSE(pin_hash::verify({.pin = "4822", .stored = one}));
  CHECK_FALSE(pin_hash::verify({.pin = "4821", .stored = "pbkdf2-sha256$10$aa$bb"}));
  CHECK_FALSE(pin_hash::verify({.pin = "4821", .stored = "garbage"}));
  CHECK(pin_hash::make({.pin = "1", .iterations = 1}).starts_with("pbkdf2-sha256$1000$"));
  CHECK(pin_hash::wellFormedPin("0000"));
  CHECK_FALSE(pin_hash::wellFormedPin("12a4"));
  CHECK_FALSE(pin_hash::wellFormedPin("123"));
}

TEST_CASE("wrong PINs lock the user for the window, then let go")
{
  PinAttempts attempts({.maxFailures = 2, .windowSeconds = 100});
  attempts.fail({.userId = 7, .now = 10});
  CHECK_FALSE(attempts.locked({.userId = 7, .now = 11}));
  attempts.fail({.userId = 7, .now = 20});
  CHECK(attempts.locked({.userId = 7, .now = 21}));
  CHECK_FALSE(attempts.locked({.userId = 8, .now = 21}));
  CHECK_FALSE(attempts.locked({.userId = 7, .now = 111}));
  attempts.fail({.userId = 7, .now = 112});
  attempts.clear(7);
  CHECK_FALSE(attempts.locked({.userId = 7, .now = 113}));
}

TEST_CASE("the two PINs must be digits and different")
{
  Json::Value body(Json::objectValue);
  body["disarmPin"] = "1234";
  body["duressPin"] = "1234";
  CHECK_THROWS_AS(SetPinDto::fromJson(body), ValidationException);
  body["duressPin"] = "12x4";
  CHECK_THROWS_AS(SetPinDto::fromJson(body), ValidationException);
  body["duressPin"] = "9876";
  CHECK(SetPinDto::fromJson(body).duressPin == "9876");
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
  CHECK_THROWS_AS(drogon::sync_wait(rig.safety.setPin({.userId = 2, .disarmPin = "1111", .duressPin = "2222"})),
                  ResponseException);
}

TEST_CASE("a duress PIN disarms like the real one and silently alerts everyone else")
{
  boot();
  SafetyRig rig;
  drogon::sync_wait(rig.safety.toggle({.duressEnabled = true, .actorUserId = 1}));
  const SafetyStatus status =
      drogon::sync_wait(rig.safety.setPin({.userId = 2, .disarmPin = "1111", .duressPin = "2222"}));
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
      drogon::sync_wait(rig.safety.setPin({.userId = 2, .disarmPin = "1111", .duressPin = "2222"})));
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
      drogon::sync_wait(rig.safety.setPin({.userId = 2, .disarmPin = "1111", .duressPin = "2222"})));
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

TEST_CASE("a panic the notification service missed is delivered on retry")
{
  boot();
  SafetyRig rig;
  rig.notifications.failNext(1);
  const PanicResult result = drogon::sync_wait(
      rig.safety.panic({.userId = 1, .userName = "Ana", .environmentId = std::nullopt}));
  CHECK_FALSE(result.sent);
  const std::string notified =
      "SELECT notified_at > 0 FROM guard_safety_alert WHERE id = " + std::to_string(result.alertId);
  for (int attempt = 0; attempt < 600 && scalar(notified) != "1"; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK_FALSE(rig.ofKind("guard_panic").empty());
  CHECK(scalar(notified) == "1");
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
                     .actorName = "Laura"};
  CHECK(guard_copy::render(notice, "es").title == "Botón de pánico · Laura");
  CHECK(guard_copy::urgency(notice) == "critical");
  notice.kind = NoticeKind::Duress;
  notice.environmentName = "Casa";
  CHECK(guard_copy::render(notice, "en").title == "Silent alert · Laura (Casa)");
  CHECK(noticeKindToString(notice.kind) == "guard_duress");
}
