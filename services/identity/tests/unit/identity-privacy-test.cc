#include <doctest/doctest.h>
#include <test-support/app-runner.hxx>

#include <app/rpc/identity-rpc-service.hxx>
#include <auth/role-access.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/privacy/dtos/response-privacy-dto.hxx>
#include <feature/privacy/dtos/update-household-privacy-dto.hxx>
#include <feature/privacy/dtos/update-minor-dto.hxx>
#include <feature/privacy/dtos/update-privacy-dto.hxx>
#include <feature/privacy/services/privacy-feature-service.hxx>
#include <feature/voiceprint/services/passive/passive-enrollment-service.hxx>
#include <grpcpp/grpcpp.h>
#include <identity/identity-client.hxx>
#include <shared/services/privacy/privacy-gate.hxx>
#include <sqlite/db-service.hxx>
#include <sync/identity-change-sink.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kIdentityDb = "identity-privacy-test.db";

using test_support::AppRunner;

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

class RecordingSink : public IdentityChangeSink
{
public:
  [[nodiscard]] drogon::Task<void>
  publishCatalog(const IdentityCatalogInput& input) const override
  {
    const std::scoped_lock lock(mutex_);
    catalog_.push_back(input.row);
    transactional_ = transactional_ && input.client != nullptr;
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  emitModule(const ModuleEmitInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishModuleAudit(const ModuleAuditInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishUsersAudit(const UserAuditInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAction(const ActionPublishInput& input) const override
  {
    const std::scoped_lock lock(mutex_);
    actions_.push_back(input.event);
    transactional_ = transactional_ && input.client != nullptr;
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishResource(const IdentityResourceInput& input) const override
  {
    const std::scoped_lock lock(mutex_);
    resources_.push_back(input);
    transactional_ = transactional_ && input.client != nullptr;
    co_return;
  }

  [[nodiscard]] std::vector<Json::Value> catalog() const
  {
    const std::scoped_lock lock(mutex_);
    return catalog_;
  }

  [[nodiscard]] std::vector<UserActionEvent> actions() const
  {
    const std::scoped_lock lock(mutex_);
    return actions_;
  }

  [[nodiscard]] std::vector<IdentityResourceInput> resources() const
  {
    const std::scoped_lock lock(mutex_);
    return resources_;
  }

  [[nodiscard]] bool transactional() const
  {
    const std::scoped_lock lock(mutex_);
    return transactional_;
  }

  void clear()
  {
    const std::scoped_lock lock(mutex_);
    catalog_.clear();
    resources_.clear();
    actions_.clear();
  }

private:
  mutable std::mutex mutex_;
  mutable std::vector<Json::Value> catalog_;
  mutable std::vector<IdentityResourceInput> resources_;
  mutable std::vector<UserActionEvent> actions_;
  mutable bool transactional_{true};
};

class RpcHarness
{
public:
  RpcHarness()
      : service_({.bus = nullptr,
                  .gate = std::make_shared<const argus::client::FleetCallerGate>(
                      argus::client::FleetGateConfig{}),
                  .auth = nullptr})
  {
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    target_ = "127.0.0.1:" + std::to_string(port);
  }

  ~RpcHarness()
  {
    if (server_)
      server_->Shutdown();
  }

  RpcHarness(const RpcHarness&) = delete;
  RpcHarness& operator=(const RpcHarness&) = delete;

  [[nodiscard]] const std::string& target() const { return target_; }

private:
  IdentityRpcService service_;
  std::unique_ptr<grpc::Server> server_;
  std::string target_;
};

void seedRows()
{
  auto client = DbService::identityClient();
  client->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active, "
      "created_at) VALUES (1, 'Ada', 'Owner', 'owner', 'es', 1, 1000)");
  client->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active, "
      "created_at) VALUES (7, 'Rita', 'Resident', 'resident', 'es', 1, 1001)");
  client->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active, "
      "created_at) VALUES (9, 'Gus', 'Guest', 'guest', 'es', 0, 1003)");
  client->execSqlSync(
      "INSERT INTO person (id, user_id, name, alias, observation, status, "
      "first_seen_at, last_seen_at, created_at) "
      "VALUES (1, 7, 'Rita', '', '', 'known', 1000, 1000, 1000)");
  client->execSqlSync(
      "INSERT INTO voice_profile (user_id, model, embedding, sample_count, "
      "speech_seconds, source, linked_at, refreshed_at) "
      "VALUES (7, 'test-model', x'00000000', 3, 20.0, 'passive', 1000, 1000)");
  client->execSqlSync(
      "INSERT INTO voice_sample (user_id, model, device_hash, embedding, "
      "turns, speech_seconds, state, created_at) "
      "VALUES (7, 'test-model', 'dev', x'00000000', 2, 6.0, 'adopted', 1000)");
}

int64_t countRows(const std::string& table, int64_t userId)
{
  const auto rows = DbService::identityClient()->execSqlSync(
      "SELECT COUNT(*) FROM " + table + " WHERE user_id = ?", userId);
  return rows.front()[0].as<int64_t>();
}

PrivacyChoices allOn()
{
  return {.presence = true,
          .faceCameras = true,
          .voiceLearning = true,
          .cameraAudio = true,
          .location = true};
}

bool hasJournalEvent(const std::vector<UserActionEvent>& actions,
                     const std::string& event)
{
  return std::ranges::any_of(actions, [&](const UserActionEvent& action) {
    return action.newData.get("event", "").asString() == event;
  });
}
}

template <typename T>
T present(const std::optional<T>& value)
{
  REQUIRE(value.has_value());
  return value.value_or(T{});
}

TEST_CASE("route access: everyone reads and decides their own choices, the "
          "owner alone reads the directory and the household switches")
{
  using role_access::hasHttpAccess;
  CHECK(hasHttpAccess({.role = UserRole::Guest,
                       .path = "/privacy/me",
                       .method = drogon::Get}));
  CHECK(hasHttpAccess({.role = UserRole::Guard,
                       .path = "/privacy/me",
                       .method = drogon::Put}));
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Resident,
                             .path = "/privacy/users",
                             .method = drogon::Get}));
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Guard,
                             .path = "/privacy/household",
                             .method = drogon::Patch}));
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Resident,
                             .path = "/privacy/me",
                             .method = drogon::Delete}));
  CHECK(hasHttpAccess({.role = UserRole::Owner,
                       .path = "/privacy/household",
                       .method = drogon::Patch}));
}

TEST_CASE("the DTOs refuse a partial decision and an unacknowledged visitor "
          "switch")
{
  Json::Value partial(Json::objectValue);
  partial["noticeVersion"] = 1;
  partial["presence"] = true;
  CHECK_THROWS_AS(UpdatePrivacyDto::fromJson(partial), ValidationException);

  Json::Value full = partial;
  full["faceCameras"] = false;
  full["voiceLearning"] = true;
  full["cameraAudio"] = false;
  const auto dto = UpdatePrivacyDto::fromJson(full);
  CHECK(dto.noticeVersion == 1);
  CHECK(dto.voiceLearning == std::optional<bool>(true));

  Json::Value visitors(Json::objectValue);
  visitors["visitorRecognition"] = true;
  CHECK_THROWS_AS(UpdateHouseholdPrivacyDto::fromJson(visitors),
                  ValidationException);
  visitors["acknowledge"] = true;
  CHECK(UpdateHouseholdPrivacyDto::fromJson(visitors).visitorRecognition ==
        std::optional<bool>(true));

  Json::Value watchman(Json::objectValue);
  watchman["watchman"] = true;
  CHECK_THROWS_AS(UpdateHouseholdPrivacyDto::fromJson(watchman), ValidationException);
  watchman["acknowledge"] = true;
  CHECK(UpdateHouseholdPrivacyDto::fromJson(watchman).watchman == std::optional<bool>(true));
  Json::Value watchmanOff(Json::objectValue);
  watchmanOff["watchman"] = false;
  CHECK(UpdateHouseholdPrivacyDto::fromJson(watchmanOff).watchman == std::optional<bool>(false));

  CHECK_FALSE(dto.location.has_value());
  full["location"] = false;
  CHECK(UpdatePrivacyDto::fromJson(full).location == std::optional<bool>(false));

  const Json::Value empty(Json::objectValue);
  CHECK_THROWS_AS(UpdateHouseholdPrivacyDto::fromJson(empty),
                  ValidationException);
}

TEST_CASE("consent is recorded, withdrawn and enforced")
{
  std::remove(kIdentityDb);
  std::remove((std::string(kIdentityDb) + "-wal").c_str());
  std::remove((std::string(kIdentityDb) + "-shm").c_str());

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                                       .filename = kIdentityDb,
                                                       .name = "default",
                                                       .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));
  seedRows();

  RecordingSink sink;
  identity_change::setSink(&sink);
  const PrivacyFeatureService service;
  const PrivacyGate gate;

  {
    INFO("an undecided person has every signal off");
    const auto view = drogon::sync_wait(service.me(7));
    CHECK_FALSE(view.state.decided);
    CHECK(view.state.effective == PrivacyChoices{});
    CHECK(view.household == allOn());
    const auto json = ResponsePrivacyDto{.view = view}.toJson();
    CHECK(json["applicable"]["presence"].asBool());
    CHECK(json["applicable"]["faceCameras"].asBool());
    CHECK(json["applicable"]["voiceLearning"].asBool());
    CHECK(json["applicable"]["cameraAudio"].asBool());
    const auto off = ResponsePrivacyDto{.view = view, .surveillanceActive = false}.toJson();
    CHECK(off["applicable"]["presence"].asBool() == false);
    CHECK(off["applicable"]["faceCameras"].asBool() == false);
    CHECK(off["applicable"]["cameraAudio"].asBool() == false);
    CHECK(off["applicable"]["voiceLearning"].asBool());
    CHECK(off["choices"] == json["choices"]);
    CHECK(off["effective"] == json["effective"]);
    CHECK(off["household"] == json["household"]);
    CHECK(json["decided"].asBool() == false);
    CHECK(json["current"].asBool() == false);
    CHECK(json["currentNoticeVersion"].asInt64() == kPrivacyNoticeVersion);
    CHECK(json["decidedAt"].isNull());
  }

  {
    INFO("a decision for an older notice is refused");
    CHECK_THROWS_AS(drogon::sync_wait(service.decide(
                        {.userId = 7, .noticeVersion = 0, .choices = allOn(), .location = std::nullopt})),
                    ResponseException);
    CHECK_FALSE(drogon::sync_wait(gate.stateFor(7)).decided);
  }

  {
    INFO("a decision is stored, published and journaled with safe metadata");
    const auto view = drogon::sync_wait(service.decide(
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = allOn(), .location = std::nullopt}));
    CHECK(view.state.decided);
    CHECK(view.state.effective == allOn());
    CHECK(countRows("voice_profile", 7) == 1);

    const auto catalog = sink.catalog();
    REQUIRE(catalog.size() == 1);
    CHECK(catalog.front()["id"].asInt64() == 7);
    CHECK(catalog.front()["privacy"]["presence"].asBool());
    CHECK(catalog.front()["privacy"]["decided"].asBool());
    CHECK(catalog.front()["privacy"]["noticeVersion"].asInt64() ==
          kPrivacyNoticeVersion);

    const auto actions = sink.actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions.front().action == UserAction::Create);
    CHECK(actions.front().newData["event"].asString() == "privacy_consent");
    CHECK(actions.front().newData["first"].asBool());
    CHECK(actions.front().ipAddress.empty());
    CHECK(sink.transactional());

    const auto notices = sink.resources();
    REQUIRE(notices.size() == 1);
    CHECK(notices.front().scope == ResourceScope::PrivacyChoice);
    CHECK(notices.front().user == 7);
    CHECK(notices.front().ids.empty());
    CHECK(notices.front().client != nullptr);
  }

  {
    INFO("withdrawing voice learning erases the learned voice in the same "
          "transaction");
    drogon::sync_wait(service.decide(
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = allOn(), .location = std::nullopt}));
    sink.clear();
    PrivacyChoices withdrawn = allOn();
    withdrawn.voiceLearning = false;
    const auto view = drogon::sync_wait(service.decide(
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = withdrawn, .location = std::nullopt}));
    CHECK_FALSE(view.state.effective.voiceLearning);
    CHECK(countRows("voice_profile", 7) == 0);
    CHECK(countRows("voice_sample", 7) == 0);

    const auto actions = sink.actions();
    CHECK(hasJournalEvent(actions, "voiceprint_forget"));
    CHECK(hasJournalEvent(actions, "privacy_consent"));
    const auto consent = std::ranges::find_if(actions, [](const auto& action) {
      return action.newData["event"].asString() == "privacy_consent";
    });
    REQUIRE(consent != actions.end());
    CHECK(consent->action == UserAction::Update);
    REQUIRE(consent->newData["changed"].size() == 1);
    CHECK(consent->newData["changed"][0].asString() == "voiceLearning");

    const auto notices = sink.resources();
    REQUIRE(notices.size() == 2);
    CHECK(notices[0].scope == ResourceScope::Voiceprints);
    CHECK(notices[0].ids == std::vector<int64_t>{7});
    CHECK(notices[1].scope == ResourceScope::PrivacyChoice);
    CHECK(notices[1].user == 7);
  }

  {
    INFO("a person without voice consent teaches nothing");
    const PassiveEnrollmentService passive;
    ClosedCall call;
    call.userId = 7;
    call.deviceHash = "dev";
    const auto outcome = drogon::sync_wait(passive.recordCall(call, 2000));
    CHECK((outcome == PassiveCallOutcome::NotConsented ||
           outcome == PassiveCallOutcome::Unavailable));
  }

  {
    INFO("the owner's household switch turns a signal off for everyone, and "
          "faces stop naming the person");
    drogon::sync_wait(service.decide(
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = allOn(), .location = std::nullopt}));
    RpcHarness harness;
    const IdentityClient client(harness.target());

    const auto named = present(client.getPerson(1));
    CHECK(named.userId == std::optional<int64_t>(7));
    CHECK(named.role == "resident");

    sink.clear();
    const auto directory = drogon::sync_wait(service.updateHousehold(
        {.actorId = 1,
         .presence = std::nullopt,
         .faceCameras = false,
         .voiceLearning = std::nullopt,
         .cameraAudio = std::nullopt,
         .location = std::nullopt,
         .visitorRecognition = std::nullopt,
         .watchman = std::nullopt}));
    CHECK_FALSE(directory.household.faceCameras);
    CHECK(directory.household.presence);
    CHECK(directory.users.size() == 2);
    const auto active = ResponsePrivacyDirectoryDto{.directory = directory}.toJson();
    const auto inactive = ResponsePrivacyDirectoryDto{.directory = directory, .surveillanceActive = false}.toJson();
    CHECK(active["applicable"]["presence"].asBool());
    CHECK(inactive["applicable"]["presence"].asBool() == false);
    CHECK(inactive["applicable"]["faceCameras"].asBool() == false);
    CHECK(inactive["applicable"]["cameraAudio"].asBool() == false);
    CHECK(inactive["applicable"]["voiceLearning"].asBool());
    CHECK(inactive["household"] == active["household"]);
    CHECK(inactive["users"] == active["users"]);
    CHECK_FALSE(drogon::sync_wait(gate.effectiveFor(7)).faceCameras);
    CHECK(drogon::sync_wait(gate.stateFor(7)).choices.faceCameras);

    const auto notices = sink.resources();
    REQUIRE(notices.size() == 2);
    CHECK(notices[0].scope == ResourceScope::PrivacyHousehold);
    CHECK(notices[0].user == 0);
    CHECK(notices[0].ids.empty());
    CHECK(notices[0].client != nullptr);
    CHECK(notices[1].scope == ResourceScope::PrivacyChoice);
    CHECK(notices[1].user == 0);

    CHECK(sink.catalog().size() == 3);
    CHECK(hasJournalEvent(sink.actions(), "household_privacy"));

    sink.clear();
    drogon::sync_wait(service.updateHousehold(
        {.actorId = 1,
         .presence = std::nullopt,
         .faceCameras = false,
         .voiceLearning = std::nullopt,
         .cameraAudio = std::nullopt,
         .location = std::nullopt,
         .visitorRecognition = std::nullopt,
         .watchman = std::nullopt}));
    CHECK(sink.resources().empty());

    const auto unnamed = present(client.getPerson(1));
    CHECK_FALSE(unnamed.userId.has_value());
    CHECK(unnamed.name.empty());
    CHECK(unnamed.role == "resident");

    const auto user = present(client.getUser(7));
    REQUIRE(user.user().has_privacy());
    CHECK(user.user().privacy().decided());
    CHECK(user.user().privacy().presence());
    CHECK_FALSE(user.user().privacy().face_cameras());

    const auto listed = present(client.listPrivacy());
    CHECK_FALSE(listed.household().face_cameras());
    CHECK_FALSE(listed.household().visitor_recognition());
    REQUIRE(listed.users_size() == 2);
    CHECK(listed.users(0).user_id() == 1);
    CHECK_FALSE(listed.users(0).choices().decided());

    const auto users = present(client.listUsers());
    REQUIRE(users.size() == 3);
    CHECK(users.at(0).user_id() == 1);
    CHECK(users.at(2).user_id() == 9);
    CHECK_FALSE(users.at(2).is_active());

    drogon::sync_wait(service.updateHousehold({.actorId = 1,
                                               .presence = std::nullopt,
                                               .faceCameras = true,
                                               .voiceLearning = std::nullopt,
                                               .cameraAudio = std::nullopt,
                                               .location = std::nullopt,
                                               .visitorRecognition = true,
                                               .watchman = std::nullopt}));
    const auto household = drogon::sync_wait(gate.household());
    CHECK(household.allowed.faceCameras);
    CHECK(household.visitorRecognition);
    CHECK(household.visitorAcknowledgedBy == std::optional<int64_t>(1));
    CHECK(household.visitorAcknowledgedAt.has_value());
  }

  {
    INFO("the watchman is the owner's acknowledged switch, and travels to the cameras");
    const auto on = drogon::sync_wait(service.updateHousehold({.actorId = 1,
                                                               .presence = std::nullopt,
                                                               .faceCameras = std::nullopt,
                                                               .voiceLearning = std::nullopt,
                                                               .cameraAudio = std::nullopt,
                                                               .location = std::nullopt,
                                                               .visitorRecognition = std::nullopt,
                                                               .watchman = true}));
    CHECK(on.watchman);
    REQUIRE(on.watchmanAcknowledgedAt.has_value());
    const auto json = ResponsePrivacyDirectoryDto{.directory = on}.toJson();
    CHECK(json["household"]["watchman"].asBool());
    CHECK(json["watchmanAcknowledgedAt"].isIntegral());
    CHECK(drogon::sync_wait(gate.household()).watchman);
    const auto off = drogon::sync_wait(service.updateHousehold({.actorId = 1,
                                                                .presence = std::nullopt,
                                                                .faceCameras = std::nullopt,
                                                                .voiceLearning = std::nullopt,
                                                                .cameraAudio = std::nullopt,
                                                                .location = std::nullopt,
                                                                .visitorRecognition = std::nullopt,
                                                                .watchman = false}));
    CHECK_FALSE(off.watchman);
  }

  {
    INFO("a location opt-out holds when a client does not send it");
    drogon::sync_wait(service.decide(
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = allOn(), .location = false}));
    CHECK_FALSE(drogon::sync_wait(gate.stateFor(7)).choices.location);
    drogon::sync_wait(service.decide(
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = allOn(), .location = std::nullopt}));
    CHECK_FALSE(drogon::sync_wait(gate.stateFor(7)).choices.location);
    CHECK_FALSE(drogon::sync_wait(gate.effectiveFor(7)).location);
    drogon::sync_wait(service.decide(
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = allOn(), .location = true}));
    CHECK(drogon::sync_wait(gate.effectiveFor(7)).location);
  }

  {
    INFO("a minor without the guardian's consent is neither recognised nor learned, and the learned voice goes");
    drogon::sync_wait(service.decide(
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = allOn(), .location = true}));
    DbService::identityClient()->execSqlSync(
        "INSERT OR REPLACE INTO voice_profile (user_id, model, embedding, sample_count, speech_seconds, source, "
        "linked_at, refreshed_at) VALUES (7, 'test-model', x'00000000', 3, 20.0, 'passive', 1000, 1000)");
    sink.clear();
    const auto marked = drogon::sync_wait(service.markMinor(
        {.actorId = 1, .userId = 7, .minor = true, .guardianConsent = false}));
    const auto rita = std::ranges::find(marked.users, int64_t{7}, &PrivacyDirectoryEntry::userId);
    REQUIRE(rita != marked.users.end());
    CHECK(rita->state.minor);
    CHECK_FALSE(rita->state.guardianConsent);
    CHECK(rita->state.choices.faceCameras);
    CHECK_FALSE(rita->state.effective.faceCameras);
    CHECK_FALSE(rita->state.effective.voiceLearning);
    CHECK(rita->state.effective.presence);
    CHECK(countRows("voice_profile", 7) == 0);
    const auto effective = drogon::sync_wait(gate.effectiveFor(7));
    CHECK_FALSE(effective.faceCameras);
    CHECK_FALSE(effective.voiceLearning);
    CHECK(drogon::sync_wait(service.me(7)).state.minor);
    CHECK(hasJournalEvent(sink.actions(), "minor_mark"));
    const auto catalog = sink.catalog();
    REQUIRE_FALSE(catalog.empty());
    CHECK(catalog.back()["privacy"]["minor"].asBool());
    CHECK_FALSE(catalog.back()["privacy"]["voiceLearning"].asBool());
    const auto json = ResponsePrivacyDirectoryDto{.directory = marked}.toJson();
    bool listed = false;
    for (const auto& user : json["users"])
      if (user["userId"].asInt64() == 7) {
        listed = true;
        CHECK(user["minor"].asBool());
        CHECK_FALSE(user["guardianConsent"].asBool());
      }
    CHECK(listed);

    RpcHarness harness;
    const IdentityClient client(harness.target());
    const auto answer = client.getUser(7);
    REQUIRE(answer.has_value());
    CHECK(answer->user().privacy().minor());
    CHECK_FALSE(answer->user().privacy().guardian_consent());
    CHECK_FALSE(answer->user().privacy().voice_learning());
  }

  {
    INFO("the guardian's consent gives the minor back their own choices, and unmarking clears the mark");
    const auto consented = drogon::sync_wait(service.markMinor(
        {.actorId = 1, .userId = 7, .minor = true, .guardianConsent = true}));
    const auto rita = std::ranges::find(consented.users, int64_t{7}, &PrivacyDirectoryEntry::userId);
    REQUIRE(rita != consented.users.end());
    CHECK(rita->state.minor);
    CHECK(rita->state.guardianConsent);
    CHECK(rita->state.effective.voiceLearning);
    CHECK(rita->state.effective.faceCameras);
    const auto guardian = DbService::identityClient()->execSqlSync(
        "SELECT guardian_id FROM user_minor WHERE user_id = 7");
    REQUIRE(guardian.size() == 1);
    CHECK(guardian.front()[0].as<int64_t>() == 1);
    drogon::sync_wait(service.markMinor({.actorId = 1, .userId = 7, .minor = false, .guardianConsent = false}));
    CHECK_FALSE(drogon::sync_wait(gate.stateFor(7)).minor);
    CHECK(countRows("user_minor", 7) == 0);
    CHECK_THROWS_AS(drogon::sync_wait(service.markMinor(
                        {.actorId = 1, .userId = 9, .minor = true, .guardianConsent = false})),
                    ResponseException);
  }
  identity_change::setSink(nullptr);
}

TEST_CASE("the minor mark is the owner's, and its body is checked")
{
  using role_access::hasHttpAccess;
  CHECK(hasHttpAccess({.role = UserRole::Owner, .path = "/privacy/users/7/minor", .method = drogon::Put}));
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Resident, .path = "/privacy/users/7/minor", .method = drogon::Put}));
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Guard, .path = "/privacy/users/7/minor", .method = drogon::Put}));

  Json::Value body(Json::objectValue);
  CHECK_THROWS_AS(UpdateMinorDto::fromJson(body, 7), ValidationException);
  body["minor"] = false;
  body["guardianConsent"] = true;
  CHECK_THROWS_AS(UpdateMinorDto::fromJson(body, 7), ValidationException);
  body["minor"] = true;
  CHECK_THROWS_AS(UpdateMinorDto::fromJson(body, 0), ValidationException);
  const auto dto = UpdateMinorDto::fromJson(body, 7);
  CHECK(dto.minor == std::optional<bool>(true));
  CHECK(dto.guardianConsent == std::optional<bool>(true));
}

TEST_CASE("a minor mark turns off the biometric signals unless the guardian consented")
{
  PrivacyState state;
  state.decided = true;
  state.choices = allOn();
  state.effective = allOn();
  CHECK(privacy_policy::withMinor(state, std::nullopt).effective == allOn());
  UserMinorSchema mark;
  mark.userId = 7;
  const PrivacyState minor = privacy_policy::withMinor(state, mark);
  CHECK(minor.minor);
  CHECK_FALSE(minor.effective.faceCameras);
  CHECK_FALSE(minor.effective.voiceLearning);
  CHECK(minor.effective.presence);
  CHECK(minor.effective.cameraAudio);
  CHECK(minor.choices == allOn());
  mark.guardianConsent = true;
  const PrivacyState consented = privacy_policy::withMinor(state, mark);
  CHECK(consented.guardianConsent);
  CHECK(consented.effective == allOn());
  CHECK(privacy_policy::toJson(minor)["minor"].asBool());
}
