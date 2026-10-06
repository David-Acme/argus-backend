#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <test-support/app-runner.hxx>

#include <app/rpc/identity-callers.hxx>
#include <app/rpc/identity-sync-rpc-service.hxx>
#include <argus/identity/v1/sync.grpc.pb.h>
#include <drogon/drogon.h>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>
#include <identity/identity-sync-client.hxx>
#include <sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kIdentityDb = "identity-sync-rpc-test.db";
constexpr const char* kFleetSecret = "identity-sync-test-secret";

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

class RpcHarness
{
public:
  explicit RpcHarness(
      std::string fleetSecret = {},
      std::vector<std::pair<std::string, std::string>> callers = {})
      : service_({.gate = std::make_shared<const argus::client::FleetCallerGate>(
                      argus::client::FleetGateConfig{
                          .expectedCallers = identity_callers::expected(),
                          .callerPairs = std::move(callers),
                          .legacySecret = std::move(fleetSecret),
                          .onFirstLegacy = {}})})
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

  [[nodiscard]] bool listening() const { return server_ != nullptr; }
  [[nodiscard]] const std::string& target() const { return target_; }

private:
  IdentitySyncRpcService service_;
  std::unique_ptr<grpc::Server> server_;
  std::string target_;
};

argus::client::CallerIdentity identityFor(int64_t userId, std::string role)
{
  return {.userId = userId, .role = std::move(role), .device = "test-device"};
}

argus::identity::v1::PullTableRequest userPull(bool deleted = false)
{
  argus::identity::v1::PullTableRequest request;
  auto* body = request.mutable_user();
  if (deleted)
    body->set_required_deleted(true);
  else
    body->set_required_create(true);
  return request;
}

argus::identity::v1::PullTableRequest invitationPull()
{
  argus::identity::v1::PullTableRequest request;
  request.mutable_user_invitation()->set_required_create(true);
  return request;
}

argus::identity::v1::PullTableRequest personPull()
{
  argus::identity::v1::PullTableRequest request;
  request.mutable_person()->set_required_create(true);
  return request;
}

argus::identity::v1::PullTableRequest lastCreatedPull()
{
  argus::identity::v1::PullTableRequest request;
  request.mutable_user()->set_find_last_created(true);
  return request;
}

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
      "created_at) VALUES (8, 'Gil', 'Guard', 'guard', 'es', 1, 1002)");
  client->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active, "
      "created_at) VALUES (9, 'Gus', 'Guest', 'guest', 'es', 1, 1003)");
  client->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active, "
      "created_at, deleted_at) VALUES (5, 'Vera', 'Gone', 'guest', 'es', 0, "
      "1004, 1500)");
  client->execSqlSync(
      "INSERT INTO user_invitation (id, token_hash, role, max_redemptions, "
      "redemption_count, expires_at, created_by, created_at) "
      "VALUES (1, 'hash-one', 'guest', 2, 0, 9000, 1, 1000)");
  client->execSqlSync(
      "INSERT INTO person (id, user_id, name, alias, observation, status, "
      "first_seen_at, last_seen_at, created_at) "
      "VALUES (1, 7, 'Ana', '', '', 'known', 1000, 1000, 1000)");
}
}

TEST_CASE("identity sync RPC gates by fleet secret, role and caller scope")
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
  seedRows();

  RpcHarness harness;
  REQUIRE(harness.listening());
  IdentitySyncClient sdk({.target = harness.target(), .fleetSecret = {}});

  {
    const auto owner = sdk.pullTable(userPull(), identityFor(1, "owner"));
    if (!owner.has_value()) {
      FAIL("the owner pull answered nothing");
      return;
    }
    REQUIRE(owner->has_user());
    REQUIRE(owner->user().created_size() == 4);

    const auto guard = sdk.pullTable(userPull(), identityFor(8, "guard"));
    if (!guard.has_value()) {
      FAIL("the guard pull answered nothing");
      return;
    }
    CHECK(guard->user().created_size() == 4);

    const auto resident = sdk.pullTable(userPull(), identityFor(7, "resident"));
    if (!resident.has_value()) {
      FAIL("the resident pull answered nothing");
      return;
    }
    REQUIRE(resident->user().created_size() == 1);
    CHECK(resident->user().created(0).id() == 7);

    const auto guest = sdk.pullTable(userPull(), identityFor(9, "guest"));
    if (!guest.has_value()) {
      FAIL("the guest pull answered nothing");
      return;
    }
    REQUIRE(guest->user().created_size() == 1);
    CHECK(guest->user().created(0).id() == 9);
  }

  {
    const auto owner = sdk.pullTable(invitationPull(), identityFor(1, "owner"));
    if (!owner.has_value()) {
      FAIL("the owner invitation pull answered nothing");
      return;
    }
    REQUIRE(owner->has_user_invitation());
    REQUIRE(owner->user_invitation().created_size() == 1);
    CHECK(owner->user_invitation().created(0).created_by() == 1);

    CHECK_FALSE(
        sdk.pullTable(invitationPull(), identityFor(8, "guard")).has_value());
    CHECK_FALSE(sdk.pullTable(invitationPull(), identityFor(7, "resident"))
                    .has_value());
    CHECK_FALSE(
        sdk.pullTable(invitationPull(), identityFor(9, "guest")).has_value());
  }

  {
    const auto resident =
        sdk.pullTable(personPull(), identityFor(7, "resident"));
    if (!resident.has_value()) {
      FAIL("the resident person pull answered nothing");
      return;
    }
    REQUIRE(resident->person().created_size() == 1);
    CHECK(resident->person().created(0).id() == 1);

    CHECK_FALSE(
        sdk.pullTable(personPull(), identityFor(9, "guest")).has_value());
  }

  {
    const auto deleted = sdk.pullTable(userPull(true), identityFor(1, "owner"));
    if (!deleted.has_value()) {
      FAIL("the owner tombstone pull answered nothing");
      return;
    }
    REQUIRE(deleted->user().deleted_size() == 1);
    CHECK(deleted->user().deleted(0).id() == 5);
    CHECK(deleted->user().deleted(0).deleted_at() == 1500);

    const auto lastRow =
        sdk.pullTable(lastCreatedPull(), identityFor(1, "owner"));
    if (!lastRow.has_value()) {
      FAIL("the owner last-created pull answered nothing");
      return;
    }
    REQUIRE(lastRow->user().has_last_created());
    CHECK(lastRow->user().last_created().id() == 9);

    argus::identity::v1::PullTableRequest lastDeleted;
    lastDeleted.mutable_user()->set_find_last_deleted(true);
    const auto tombstone = sdk.pullTable(lastDeleted, identityFor(1, "owner"));
    if (!tombstone.has_value()) {
      FAIL("the owner last-deleted pull answered nothing");
      return;
    }
    REQUIRE(tombstone->user().has_last_deleted());
    CHECK(tombstone->user().last_deleted().id() == 5);
  }

  {
    const auto own =
        sdk.pullTable(lastCreatedPull(), identityFor(7, "resident"));
    if (!own.has_value()) {
      FAIL("the resident last-created pull answered nothing");
      return;
    }
    REQUIRE(own->user().has_last_created());
    CHECK(own->user().last_created().id() == 7);

    const auto ownTombstone =
        sdk.pullTable(userPull(true), identityFor(9, "guest"));
    if (!ownTombstone.has_value()) {
      FAIL("the guest tombstone pull answered nothing");
      return;
    }
    CHECK(ownTombstone->user().deleted_size() == 0);
  }

  {
    argus::identity::v1::PullTableRequest empty;
    CHECK_FALSE(sdk.pullTable(empty, identityFor(1, "owner")).has_value());
  }

  {
    RpcHarness gated(kFleetSecret);
    REQUIRE(gated.listening());

    IdentitySyncClient withSecret(
        {.target = gated.target(), .fleetSecret = kFleetSecret});
    const auto served =
        withSecret.pullTable(userPull(), identityFor(1, "owner"));
    if (!served.has_value()) {
      FAIL("the fleet-secret pull answered nothing");
      return;
    }
    CHECK(served->user().created_size() == 4);

    IdentitySyncClient wrongSecret(
        {.target = gated.target(), .fleetSecret = "not-the-secret"});
    CHECK_FALSE(
        wrongSecret.pullTable(userPull(), identityFor(1, "owner")).has_value());

    auto raw = argus::identity::v1::SyncService::NewStub(
        argus::client::makeChannel(gated.target()));
    grpc::ClientContext context;
    argus::identity::v1::PullTableResponse response;
    const grpc::Status status = raw->PullTable(&context, userPull(), &response);
    CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
  }

  {
    std::vector<std::pair<std::string, std::string>> callers;
    for (const auto& caller : identity_callers::expected())
      callers.emplace_back(caller, caller + "-identity-credential");
    RpcHarness paired(kFleetSecret, callers);
    REQUIRE(paired.listening());

    IdentitySyncClient sync({.target = paired.target(),
                             .credential = "sync-identity-credential",
                             .fleetSecret = {}});
    const auto served = sync.pullTable(userPull(), identityFor(1, "owner"));
    if (!served.has_value()) {
      FAIL("the paired sync pull answered nothing");
      return;
    }
    CHECK(served->user().created_size() == 4);

    IdentitySyncClient camera({.target = paired.target(),
                               .credential = "camera-identity-credential",
                               .fleetSecret = {}});
    CHECK_FALSE(camera.pullTable(userPull(), identityFor(1, "owner")).has_value());

    IdentitySyncClient legacy({.target = paired.target(),
                               .credential = {},
                               .fleetSecret = kFleetSecret});
    CHECK_FALSE(legacy.pullTable(userPull(), identityFor(1, "owner")).has_value());

    auto raw = argus::identity::v1::SyncService::NewStub(
        argus::client::makeChannel(paired.target()));
    grpc::ClientContext context;
    argus::client::addCallerCredential(context, "camera-identity-credential");
    argus::client::addCallerIdentity(context, identityFor(1, "owner"));
    argus::identity::v1::PullTableResponse response;
    CHECK(raw->PullTable(&context, userPull(), &response).error_code() ==
          grpc::StatusCode::PERMISSION_DENIED);
  }

  std::remove(kIdentityDb);
  std::remove((std::string(kIdentityDb) + "-wal").c_str());
  std::remove((std::string(kIdentityDb) + "-shm").c_str());
}
