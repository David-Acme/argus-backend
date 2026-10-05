#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/identity-rpc-service.hxx>
#include <auth/role-access.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/privacy/dtos/response-privacy-dto.hxx>
#include <feature/privacy/dtos/update-household-privacy-dto.hxx>
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
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kIdentityDb = "identity-privacy-test.db";

class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::thread runner_;
};

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

  [[nodiscard]] bool transactional() const
  {
    const std::scoped_lock lock(mutex_);
    return transactional_;
  }

  void clear()
  {
    const std::scoped_lock lock(mutex_);
    catalog_.clear();
    actions_.clear();
  }

private:
  mutable std::mutex mutex_;
  mutable std::vector<Json::Value> catalog_;
  mutable std::vector<UserActionEvent> actions_;
  mutable bool transactional_{true};
};

class RpcHarness
{
public:
  RpcHarness() : service_({.bus = nullptr, .fleetSecret = {}, .auth = nullptr})
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
          .cameraAudio = true};
}

bool hasJournalEvent(const std::vector<UserActionEvent>& actions,
                     const std::string& event)
{
  return std::ranges::any_of(actions, [&](const UserActionEvent& action) {
    return action.newData.get("event", "").asString() == event;
  });
}
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
    CHECK(json["decided"].asBool() == false);
    CHECK(json["current"].asBool() == false);
    CHECK(json["currentNoticeVersion"].asInt64() == kPrivacyNoticeVersion);
    CHECK(json["decidedAt"].isNull());
  }

  {
    INFO("a decision for an older notice is refused");
    CHECK_THROWS_AS(drogon::sync_wait(service.decide(
                        {.userId = 7, .noticeVersion = 0, .choices = allOn()})),
                    ResponseException);
    CHECK_FALSE(drogon::sync_wait(gate.stateFor(7)).decided);
  }

  {
    INFO("a decision is stored, published and journaled with safe metadata");
    const auto view = drogon::sync_wait(service.decide(
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = allOn()}));
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
  }

  {
    INFO("withdrawing voice learning erases the learned voice in the same "
          "transaction");
    drogon::sync_wait(service.decide(
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = allOn()}));
    sink.clear();
    PrivacyChoices withdrawn = allOn();
    withdrawn.voiceLearning = false;
    const auto view = drogon::sync_wait(service.decide(
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = withdrawn}));
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
        {.userId = 7, .noticeVersion = kPrivacyNoticeVersion, .choices = allOn()}));
    RpcHarness harness;
    const IdentityClient client(harness.target());

    const auto named = client.getPerson(1);
    REQUIRE(named.has_value());
    CHECK(named.value().userId == std::optional<int64_t>(7));
    CHECK(named.value().role == "resident");

    sink.clear();
    const auto directory = drogon::sync_wait(service.updateHousehold(
        {.actorId = 1,
         .presence = std::nullopt,
         .faceCameras = false,
         .voiceLearning = std::nullopt,
         .cameraAudio = std::nullopt,
         .visitorRecognition = std::nullopt}));
    CHECK_FALSE(directory.household.faceCameras);
    CHECK(directory.household.presence);
    CHECK(directory.users.size() == 2);
    CHECK_FALSE(drogon::sync_wait(gate.effectiveFor(7)).faceCameras);
    CHECK(drogon::sync_wait(gate.stateFor(7)).choices.faceCameras);
    CHECK(sink.catalog().size() == 3);
    CHECK(hasJournalEvent(sink.actions(), "household_privacy"));

    const auto unnamed = client.getPerson(1);
    REQUIRE(unnamed.has_value());
    CHECK_FALSE(unnamed.value().userId.has_value());
    CHECK(unnamed.value().name.empty());
    CHECK(unnamed.value().role == "resident");

    const auto user = client.getUser(7);
    REQUIRE(user.has_value());
    REQUIRE(user.value().user().has_privacy());
    CHECK(user.value().user().privacy().decided());
    CHECK(user.value().user().privacy().presence());
    CHECK_FALSE(user.value().user().privacy().face_cameras());

    const auto listed = client.listPrivacy();
    REQUIRE(listed.has_value());
    CHECK_FALSE(listed.value().household().face_cameras());
    CHECK_FALSE(listed.value().household().visitor_recognition());
    REQUIRE(listed.value().users_size() == 2);
    CHECK(listed.value().users(0).user_id() == 1);
    CHECK_FALSE(listed.value().users(0).choices().decided());

    const auto users = client.listUsers();
    REQUIRE(users.has_value());
    REQUIRE(users.value().size() == 3);
    CHECK(users.value().at(0).user_id() == 1);
    CHECK(users.value().at(2).user_id() == 9);
    CHECK_FALSE(users.value().at(2).is_active());

    drogon::sync_wait(service.updateHousehold({.actorId = 1,
                                               .presence = std::nullopt,
                                               .faceCameras = true,
                                               .voiceLearning = std::nullopt,
                                               .cameraAudio = std::nullopt,
                                               .visitorRecognition = true}));
    const auto household = drogon::sync_wait(gate.household());
    CHECK(household.allowed.faceCameras);
    CHECK(household.visitorRecognition);
    CHECK(household.visitorAcknowledgedBy == std::optional<int64_t>(1));
    CHECK(household.visitorAcknowledgedAt.has_value());
  }

  identity_change::setSink(nullptr);
}
