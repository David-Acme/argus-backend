#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "guard-fakes.hxx"

#include <app/rpc/duress-rpc-service.hxx>
#include <doctest/doctest.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/guard/guard-service.hxx>
#include <feature/guard/infra/resource-change-outbox.hxx>
#include <feature/safety/dtos/update-safety-dto.hxx>
#include <feature/safety/infra/guard-alert-sink.hxx>
#include <feature/safety/infra/notification-actor-notifier.hxx>
#include <feature/safety/services/safety-service.hxx>
#include <guard/guard-duress-client.hxx>
#include <nats/nats-bus.hxx>
#include <text/json-util.hxx>
#include <text/secret-phrase.hxx>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using guard_test::GuardBoot;
using guard_test::QuietCameraActions;
using guard_test::RosterIdentity;
using guard_test::scalar;

namespace
{

constexpr const char* kPhrase = "El gato está en el tejado";
constexpr const char* kLlmCredential = "llm-duress-credential";

GuardBoot& boot()
{
  static GuardBoot shared("guard-duress-phrase-test");
  return shared;
}

struct Delivered
{
  std::string title;
  std::string body;
  Json::Value data;
  std::vector<int64_t> userIds;
};

class RecordedNotifications final : public NotificationClient
{
public:
  RecordedNotifications() : NotificationClient({.target = "127.0.0.1:1", .credential = {}}) {}

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::client::CallerIdentity&) const override
  {
    const std::scoped_lock lock(mutex_);
    sent_.push_back({.title = request.title(),
                     .body = request.body(),
                     .data = json_util::fromString(request.data()),
                     .userIds = {request.user_ids().begin(), request.user_ids().end()}});
    NotificationCreateResult result;
    result.outcome = NotificationRpcOutcome::Success;
    result.created = request.user_ids_size();
    return result;
  }

  [[nodiscard]] std::vector<Delivered> ofKind(const std::string& kind) const
  {
    const std::scoped_lock lock(mutex_);
    std::vector<Delivered> found;
    for (const auto& row : sent_)
      if (row.data["kind"].asString() == kind)
        found.push_back(row);
    return found;
  }

  [[nodiscard]] size_t count() const
  {
    const std::scoped_lock lock(mutex_);
    return sent_.size();
  }

private:
  mutable std::mutex mutex_;
  mutable std::vector<Delivered> sent_;
};

class ScriptedActors final : public SafetyActorDirectory
{
public:
  explicit ScriptedActors(std::map<int64_t, SafetyActor> actors) : actors_(std::move(actors)) {}

  [[nodiscard]] drogon::Task<SafetyActor> actorOf(int64_t userId) const override
  {
    const auto found = actors_.find(userId);
    co_return found == actors_.end() ? SafetyActor{} : found->second;
  }

private:
  std::map<int64_t, SafetyActor> actors_;
};

GuardService::Config quickRetries()
{
  GuardService::Config config;
  config.retryBaseMs = 1;
  config.retryMaxMs = 1;
  return config;
}

struct PhraseRig
{
  PhraseRig()
  {
    scalar("DELETE FROM guard_user_pin");
    scalar("DELETE FROM guard_safety_setting");
    scalar("DELETE FROM guard_safety_alert");
    scalar("DELETE FROM change_outbox");
  }

  RosterIdentity identity{std::map<int64_t, std::string>{{1, "es"}, {2, "es"}, {3, "en"}}};
  RecordedNotifications notifications;
  QuietCameraActions camera;
  std::shared_ptr<NatsBus> bus{std::make_shared<NatsBus>()};
  ResourceChangeOutbox changes{bus};
  GuardService guard{GuardService::Dependencies{.bus = nullptr,
                                                .identity = &identity,
                                                .notifications = &notifications,
                                                .actions = &camera,
                                                .assessment = nullptr,
                                                .directory = {},
                                                .active = {},
                                                .episodeChanges = nullptr},
                     quickRetries()};
  GuardAlertSink sink{guard};
  NotificationActorNotifier actor{
      NotificationActorNotifier::Dependencies{.notifications = &notifications, .identity = &identity}};
  int64_t clock{1'700'000'000};
  std::shared_ptr<SafetyService> safety{std::make_shared<SafetyService>(
      SafetyService::Dependencies{.sink = &sink, .actor = &actor, .changes = &changes, .clock = [this] { return clock; }},
      SafetyService::Config{.pinIterations = 1000,
                            .panicRepeatWindowS = 60,
                            .panicLimit = 3,
                            .panicLimitWindowS = 3600,
                            .retryBaseS = 2,
                            .retryMaxS = 8,
                            .resumeWindowS = 86400,
                            .sweepIntervalS = 5.0,
                            .retentionS = 30LL * 86400,
                            .attempts = {.maxFailures = 3, .windowSeconds = 900},
                            .phraseIterations = secret_phrase::kMinIterations,
                            .spokenRepeatWindowS = 60})};

  SafetySetting update(const SafetyUpdateInput& input) const { return drogon::sync_wait(safety->update(input)); }

  SafetySetting setPhrase(const std::string& phrase, std::optional<std::string> currentPin = std::nullopt) const
  {
    return update({.toggle = {.duressEnabled = true, .actorUserId = 1, .actorName = "Ana", .currentPin = std::move(currentPin)},
                   .phraseChange = PhraseChange::Set,
                   .phrase = phrase});
  }

  [[nodiscard]] std::string storedKey() const
  {
    return scalar("SELECT duress_phrase_key FROM guard_safety_setting WHERE id = 1");
  }

  void waitForKind(const std::string& kind, size_t count) const
  {
    for (int attempt = 0; attempt < 300 && notifications.ofKind(kind).size() < count; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  void waitForNotified(size_t count) const
  {
    const std::string expected = std::to_string(count);
    for (int attempt = 0; attempt < 300 &&
                          scalar("SELECT COUNT(*) FROM guard_safety_alert WHERE notified_at > 0") != expected;
         ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
};

Json::Value body(const std::optional<Json::Value>& phrase, bool enabled = true)
{
  Json::Value json(Json::objectValue);
  json["duressEnabled"] = enabled;
  if (phrase)
    json["duressPhrase"] = *phrase;
  return json;
}

int statusOf(const std::function<void()>& call)
{
  try {
    call();
  }
  catch (const ResponseException& error) {
    return error.statusCode();
  }
  catch (const ValidationException& error) {
    return error.statusCode();
  }
  return 0;
}

bool contains(const std::vector<int64_t>& ids, int64_t id)
{
  return std::ranges::find(ids, id) != ids.end();
}

}

TEST_CASE("the phrase is validated like the other safety settings")
{
  CHECK(UpdateSafetyDto::fromJson(body(std::nullopt)).phraseChange == PhraseChange::Keep);
  CHECK(UpdateSafetyDto::fromJson(body(Json::Value(Json::nullValue))).phraseChange == PhraseChange::Clear);
  CHECK(UpdateSafetyDto::fromJson(body(Json::Value("   "))).phraseChange == PhraseChange::Clear);
  const UpdateSafetyDto set = UpdateSafetyDto::fromJson(body(Json::Value(kPhrase)));
  CHECK(set.phraseChange == PhraseChange::Set);
  CHECK(set.duressPhrase == kPhrase);
  CHECK_THROWS_AS(UpdateSafetyDto::fromJson(body(Json::Value("dos palabras"))), ValidationException);
  CHECK_THROWS_AS(UpdateSafetyDto::fromJson(body(Json::Value("tengo 3 gatos negros"))), ValidationException);
  CHECK_THROWS_AS(UpdateSafetyDto::fromJson(body(Json::Value("uno dos tres\ncuatro"))), ValidationException);
  CHECK_THROWS_AS(
      UpdateSafetyDto::fromJson(body(Json::Value("a b c d e f g h i j k l m"))), ValidationException);
  CHECK_THROWS_AS(UpdateSafetyDto::fromJson(body(Json::Value(kPhrase), false)), ValidationException);
  CHECK_THROWS_AS(UpdateSafetyDto::fromJson(body(Json::Value(42))), ValidationException);
}

TEST_CASE("the phrase is stored only as a salted key and never returned")
{
  boot();
  PhraseRig rig;
  const SafetySetting setting = rig.setPhrase(kPhrase);
  CHECK(setting.duressEnabled);
  const std::string stored = rig.storedKey();
  CHECK(stored.starts_with("phrase-pbkdf2-sha256$1024$6$"));
  CHECK(stored.find("gato") == std::string::npos);
  CHECK(stored.find("tejado") == std::string::npos);
  CHECK(drogon::sync_wait(rig.safety->phraseKey()) == stored);
  REQUIRE(secret_phrase::decode(stored).has_value());
  const secret_phrase::PhraseKey key = secret_phrase::decode(stored).value_or(secret_phrase::PhraseKey{});
  CHECK(secret_phrase::spokenIn(key, "Argus, el gato está en el tejado."));
  const SafetyStatus status = drogon::sync_wait(rig.safety->status(2));
  CHECK(status.duressEnabled);
  CHECK(status.hasPhrase);
  CHECK(scalar("SELECT COUNT(*) FROM change_outbox WHERE payload LIKE '%gato%'") == "0");

  rig.setPhrase("la tía Rosa no viene");
  CHECK(rig.storedKey() != stored);
  CHECK(rig.storedKey().starts_with("phrase-pbkdf2-sha256$1024$5$"));
}

TEST_CASE("clearing the phrase or switching duress off forgets it")
{
  boot();
  PhraseRig rig;
  rig.setPhrase(kPhrase);
  rig.update({.toggle = {.duressEnabled = true, .actorUserId = 1, .actorName = "Ana", .currentPin = std::nullopt},
              .phraseChange = PhraseChange::Clear,
              .phrase = {}});
  CHECK(rig.storedKey().empty());
  CHECK_FALSE(drogon::sync_wait(rig.safety->status(1)).hasPhrase);

  rig.setPhrase(kPhrase);
  static_cast<void>(drogon::sync_wait(rig.safety->toggle(
      {.duressEnabled = false, .actorUserId = 1, .actorName = "Ana", .currentPin = std::nullopt})));
  CHECK(rig.storedKey().empty());
  CHECK(drogon::sync_wait(rig.safety->phraseKey()).empty());
  CHECK(statusOf([&rig] {
          static_cast<void>(rig.update(
              {.toggle = {.duressEnabled = false, .actorUserId = 1, .actorName = "Ana", .currentPin = std::nullopt},
               .phraseChange = PhraseChange::Set,
               .phrase = kPhrase}));
        }) == 409);
}

TEST_CASE("changing the phrase asks the caller's own code, and the duress code still alerts")
{
  boot();
  PhraseRig rig;
  rig.setPhrase(kPhrase);
  static_cast<void>(drogon::sync_wait(rig.safety->setPin(
      {.userId = 1, .userName = "Ana", .disarmPin = "4721", .duressPin = "9386", .currentPin = std::nullopt})));
  const std::string before = rig.storedKey();
  CHECK(statusOf([&rig] { static_cast<void>(rig.setPhrase("la tía Rosa no viene")); }) == 403);
  CHECK(statusOf([&rig] { static_cast<void>(rig.setPhrase("la tía Rosa no viene", "1111")); }) == 403);
  CHECK(rig.storedKey() == before);
  rig.setPhrase("la tía Rosa no viene", "4721");
  CHECK(rig.storedKey() != before);
  CHECK(rig.notifications.ofKind("guard_duress").empty());

  rig.setPhrase(kPhrase, "9386");
  rig.waitForKind("guard_duress", 1);
  rig.waitForNotified(1);
  CHECK_FALSE(rig.notifications.ofKind("guard_duress").empty());
  CHECK(scalar("SELECT spoken FROM guard_safety_alert WHERE kind = 'duress' AND user_id = 1") == "0");
}

TEST_CASE("a spoken duress raises the same silent alert, worded for the phrase")
{
  boot();
  PhraseRig rig;
  rig.setPhrase(kPhrase);
  const std::string key = rig.storedKey();
  CHECK(drogon::sync_wait(rig.safety->spokenDuress({.userId = 2, .userName = "Laura", .phraseKey = key})) ==
        SpokenDuressOutcome::Raised);
  rig.waitForKind("guard_duress", 2);
  rig.waitForNotified(1);
  const auto alerts = rig.notifications.ofKind("guard_duress");
  REQUIRE_FALSE(alerts.empty());
  std::vector<int64_t> recipients;
  for (const auto& alert : alerts) {
    recipients.insert(recipients.end(), alert.userIds.begin(), alert.userIds.end());
    CHECK(alert.data["urgency"].asString() == "critical");
    CHECK(alert.data["actorUserId"].asInt64() == 2);
    CHECK(alert.data["threadKey"].asString().starts_with("guard:duress:"));
    CHECK(alert.title.find("Laura") != std::string::npos);
    const bool english = alert.data["lang"].asString() == "en";
    CHECK(alert.body.find(english ? "duress phrase" : "frase de coacción") != std::string::npos);
    CHECK(alert.body.find("gato") == std::string::npos);
  }
  CHECK_FALSE(contains(recipients, 2));
  CHECK(contains(recipients, 1));
  CHECK(contains(recipients, 3));
  CHECK(rig.notifications.ofKind("guard_panic_sent").empty());
  CHECK(scalar("SELECT COUNT(*) FROM guard_safety_alert WHERE kind = 'duress' AND spoken = 1 AND user_id = 2 "
               "AND notified_at > 0") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_incident WHERE rule IN ('duress', 'panic')") == "0");
}

TEST_CASE("a spoken duress repeated within a minute folds into the first")
{
  boot();
  PhraseRig rig;
  rig.setPhrase(kPhrase);
  const std::string key = rig.storedKey();
  CHECK(drogon::sync_wait(rig.safety->spokenDuress({.userId = 2, .userName = "Laura", .phraseKey = key})) ==
        SpokenDuressOutcome::Raised);
  rig.clock += 30;
  CHECK(drogon::sync_wait(rig.safety->spokenDuress({.userId = 2, .userName = "Laura", .phraseKey = key})) ==
        SpokenDuressOutcome::Repeated);
  CHECK(scalar("SELECT COUNT(*) FROM guard_safety_alert WHERE spoken = 1") == "1");
  rig.clock += 31;
  CHECK(drogon::sync_wait(rig.safety->spokenDuress({.userId = 2, .userName = "Laura", .phraseKey = key})) ==
        SpokenDuressOutcome::Raised);
  CHECK(scalar("SELECT COUNT(*) FROM guard_safety_alert WHERE spoken = 1") == "2");
  rig.waitForNotified(2);
}

TEST_CASE("a stale key, or duress switched off, raises nothing")
{
  boot();
  PhraseRig rig;
  rig.setPhrase(kPhrase);
  const std::string old = rig.storedKey();
  rig.setPhrase("la tía Rosa no viene");
  CHECK(drogon::sync_wait(rig.safety->spokenDuress({.userId = 2, .userName = "Laura", .phraseKey = old})) ==
        SpokenDuressOutcome::Stale);
  const std::string current = rig.storedKey();
  static_cast<void>(drogon::sync_wait(rig.safety->toggle(
      {.duressEnabled = false, .actorUserId = 1, .actorName = "Ana", .currentPin = std::nullopt})));
  CHECK(drogon::sync_wait(rig.safety->spokenDuress({.userId = 2, .userName = "Laura", .phraseKey = current})) ==
        SpokenDuressOutcome::Stale);
  CHECK(scalar("SELECT COUNT(*) FROM guard_safety_alert") == "0");
  CHECK(rig.notifications.count() == 0);
}

TEST_CASE("the duress RPC answers only the llm caller and checks the speaker's capability")
{
  boot();
  PhraseRig rig;
  rig.setPhrase(kPhrase);
  const std::string key = rig.storedKey();
  const ScriptedActors actors(std::map<int64_t, SafetyActor>{
      {2, {.known = true, .active = true, .name = "Laura", .role = "resident"}},
      {3, {.known = true, .active = true, .name = "Marta", .role = "guest"}},
      {4, {.known = true, .active = false, .name = "Pablo", .role = "owner"}}});
  DuressRpcService rpc({.safety = rig.safety,
                        .actors = &actors,
                        .credentials = {{.service = "llm", .secret = kLlmCredential}}});
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&rpc);
  const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  REQUIRE(server);
  const std::string target = "127.0.0.1:" + std::to_string(port);

  const GuardDuressClient stranger({.target = target, .credential = "not-the-llm"});
  CHECK_FALSE(stranger.phraseKey().has_value());
  CHECK(stranger.raiseSpoken({.userId = 2, .role = "resident", .key = key}) == SpokenDuressReply::Unavailable);

  const GuardDuressClient llm({.target = target, .credential = kLlmCredential});
  CHECK(llm.phraseKey() == std::optional<std::string>(key));
  CHECK(llm.raiseSpoken({.userId = 3, .role = "owner", .key = key}) == SpokenDuressReply::NotAllowed);
  CHECK(llm.raiseSpoken({.userId = 4, .role = "owner", .key = key}) == SpokenDuressReply::NotAllowed);
  CHECK(llm.raiseSpoken({.userId = 5, .role = "guest", .key = key}) == SpokenDuressReply::NotAllowed);
  CHECK(llm.raiseSpoken({.userId = 2, .role = "guest", .key = "phrase-pbkdf2-sha256$1024$6$00$00"}) ==
        SpokenDuressReply::Stale);
  CHECK(scalar("SELECT COUNT(*) FROM guard_safety_alert") == "0");
  CHECK(llm.raiseSpoken({.userId = 2, .role = "guest", .key = key}) == SpokenDuressReply::Raised);
  CHECK(scalar("SELECT actor_name FROM guard_safety_alert WHERE spoken = 1 AND user_id = 2") == "Laura");
  rig.waitForNotified(1);
  server->Shutdown();
}
