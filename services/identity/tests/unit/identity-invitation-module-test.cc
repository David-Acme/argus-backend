#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <test-support/app-runner.hxx>

#include <auth/module-gate.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/enrollment/services/invitation-redemption.hxx>
#include <feature/invitation/dtos/response-invitation-dto.hxx>
#include <feature/invitation/services/invitation-module-revocation.hxx>
#include <feature/invitation/services/invitation-standing.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <identity/identity-errors.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <optional>
#include <variant>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kDb = "identity-invitation-module-test.db";

using test_support::AppRunner;

bool waitForBoot()
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

void boot()
{
  static const auto runner = [] {
    for (const char* suffix : {"", "-wal", "-shm"})
      std::remove((std::string(kDb) + suffix).c_str());
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(drogon::orm::Sqlite3Config{
        .connectionNumber = 1, .filename = kDb, .name = "default", .timeout = -1});
    return std::make_unique<AppRunner>();
  }();
  REQUIRE(runner != nullptr);
  REQUIRE(waitForBoot());
  static const bool schema = [] {
    DbService::installExtensions();
    return DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA);
  }();
  REQUIRE(schema);
}

int64_t scalar(std::string_view sql)
{
  const auto rows = DbService::client()->execSqlSync(std::string(sql));
  return rows.empty() ? -1 : rows.front()[0].as<int64_t>();
}

std::string text(std::string_view sql)
{
  const auto rows = DbService::client()->execSqlSync(std::string(sql));
  if (rows.empty() || rows.front()[0].isNull())
    return "<null>";
  return rows.front()[0].as<std::string>();
}

ModuleFlag surveillance(bool enabled)
{
  return {.id = "surveillance",
          .enabled = enabled,
          .lifecycle = enabled ? "active" : "disabled",
          .dataPurgedAt = 0,
          .roles = {"guard"},
          .name = {.es = "Vigilancia", .en = "Surveillance"},
          .summary = {},
          .intro = {}};
}

class RecordingSink : public IdentityChangeSink
{
public:
  struct Published
  {
    int64_t recordId{0};
    std::optional<int64_t> actorId;
    Json::Value after;
  };

  [[nodiscard]] drogon::Task<void> publishCatalog(const IdentityCatalogInput&) const override { co_return; }
  [[nodiscard]] drogon::Task<void> emitModule(const ModuleEmitInput&) const override { co_return; }

  [[nodiscard]] drogon::Task<void> publishModuleAudit(const ModuleAuditInput& input) const override
  {
    audits_.push_back({.recordId = input.recordId, .actorId = input.actorId, .after = input.after});
    co_return;
  }

  [[nodiscard]] drogon::Task<void> publishUsersAudit(const UserAuditInput&) const override { co_return; }

  [[nodiscard]] drogon::Task<void> publishAction(const ActionPublishInput& input) const override
  {
    actions_.push_back({.recordId = input.event.recordId, .actorId = input.event.userId, .after = input.event.newData});
    co_return;
  }

  [[nodiscard]] const std::vector<Published>& audits() const { return audits_; }
  [[nodiscard]] const std::vector<Published>& actions() const { return actions_; }

private:
  mutable std::vector<Published> audits_;
  mutable std::vector<Published> actions_;
};

struct InvitationRow
{
  std::string hash;
  std::string role;
  int redeemed{0};
  int64_t expiresAt{0};
  int revoked{0};
};

int64_t insertInvitation(const InvitationRow& row)
{
  const auto result = DbService::client()->execSqlSync(
      "INSERT INTO user_invitation (token_hash, role, max_redemptions, redemption_count, expires_at, "
      "created_by, revoked_at) VALUES (?, ?, 1, ?, ?, 71, CASE WHEN ? THEN strftime('%s', 'now') END)",
      row.hash, row.role, row.redeemed, row.expiresAt, row.revoked);
  return static_cast<int64_t>(result.insertId());
}

std::optional<UserInvitationSchema> stored(std::string_view hash)
{
  return drogon::sync_wait(UserInvitationRepository{}.findByTokenHash(std::string(hash)));
}

drogon::Task<InvitationRedemptionResult> redeem(std::string hash, UserRole role)
{
  auto transaction = co_await db_transaction::begin(DbService::identityClient());
  const InvitationRedemption redemption;
  const auto result = co_await redemption.consume(
      {.tokenHash = std::move(hash), .role = role, .now = static_cast<int64_t>(std::time(nullptr)), .client = transaction.get()});
  if (result.status == RedemptionStatus::Consumed)
    co_await db_transaction::Commit(std::move(transaction));
  else
    db_transaction::rollback(transaction);
  co_return result;
}

void seedOwner()
{
  DbService::client()->execSqlSync("INSERT OR IGNORE INTO user (id, name, last_name, role) VALUES (71, 'Olga', '', 'owner')");
}
}

TEST_CASE("a module that goes off revokes the pending invitations of its roles, with the reason, and nothing else")
{
  boot();
  seedOwner();
  const int64_t later = static_cast<int64_t>(std::time(nullptr)) + 600;
  const int64_t pendingGuard = insertInvitation({.hash = "r-guard", .role = "guard", .redeemed = 0, .expiresAt = later, .revoked = 0});
  insertInvitation({.hash = "r-expired", .role = "guard", .redeemed = 0, .expiresAt = 1, .revoked = 0});
  insertInvitation({.hash = "r-spent", .role = "guard", .redeemed = 1, .expiresAt = later, .revoked = 0});
  insertInvitation({.hash = "r-manual", .role = "guard", .redeemed = 0, .expiresAt = later, .revoked = 1});
  insertInvitation({.hash = "r-resident", .role = "resident", .redeemed = 0, .expiresAt = later, .revoked = 0});

  RecordingSink sink;
  identity_change::setSink(&sink);
  moduleGate().reset();
  moduleGate().apply({surveillance(false)});

  CHECK(drogon::sync_wait(InvitationModuleRevocation{}.revokeModule("surveillance")) == 1);

  const auto revoked = stored("r-guard");
  REQUIRE(revoked.has_value());
  CHECK(revoked.value_or(UserInvitationSchema{}).revokedAt.has_value());
  CHECK_FALSE(revoked.value_or(UserInvitationSchema{}).revokedBy.has_value());
  CHECK(revoked.value_or(UserInvitationSchema{}).revokedReason == InvitationRevocationReason::ModuleDisabled);
  CHECK(revoked.value_or(UserInvitationSchema{}).revokedModule == "surveillance");
  const auto json = revoked.value_or(UserInvitationSchema{}).toJson();
  CHECK(json["revokedReason"].asString() == "module_disabled");
  CHECK(json["revokedModule"].asString() == "surveillance");
  CHECK(json["revokedAt"].isInt64());
  const auto listed = ResponseInvitationDto{.invitation = revoked.value_or(UserInvitationSchema{}), .token = ""}.toJson();
  CHECK(listed["revokedReason"].asString() == "module_disabled");
  CHECK(listed["revokedModule"].asString() == "surveillance");
  CHECK(listed["revokedAt"].isInt64());
  const auto open = ResponseInvitationDto{.invitation = stored("r-resident").value_or(UserInvitationSchema{}), .token = ""}.toJson();
  CHECK(open["revokedReason"].isNull());
  CHECK(open["revokedModule"].isNull());

  CHECK(text("SELECT revoked_reason FROM user_invitation WHERE token_hash = 'r-expired'") == "<null>");
  CHECK(text("SELECT revoked_reason FROM user_invitation WHERE token_hash = 'r-spent'") == "<null>");
  CHECK(text("SELECT revoked_reason FROM user_invitation WHERE token_hash = 'r-manual'") == "<null>");
  CHECK(text("SELECT revoked_reason FROM user_invitation WHERE token_hash = 'r-resident'") == "<null>");
  CHECK(scalar("SELECT COUNT(*) FROM user_invitation WHERE token_hash = 'r-resident' AND revoked_at IS NULL") == 1);

  REQUIRE(sink.audits().size() == 1);
  CHECK(sink.audits().front().recordId == pendingGuard);
  CHECK_FALSE(sink.audits().front().actorId.has_value());
  CHECK(sink.audits().front().after["revokedReason"].asString() == "module_disabled");
  REQUIRE(sink.actions().size() == 1);
  CHECK(sink.actions().front().recordId == pendingGuard);
  CHECK(sink.actions().front().actorId == std::optional<int64_t>(0));

  CHECK(drogon::sync_wait(InvitationModuleRevocation{}.revokeModule("surveillance")) == 0);
  CHECK(sink.audits().size() == 1);

  moduleGate().apply({surveillance(true)});
  const auto stillRevoked = invitation_standing::assess(stored("r-guard"), static_cast<int64_t>(std::time(nullptr)));
  CHECK(stillRevoked.standing == InvitationStanding::ModuleDisabled);
  CHECK(stillRevoked.moduleId == "surveillance");
  CHECK(invitation_standing::assess(stored("r-resident"), static_cast<int64_t>(std::time(nullptr))).standing ==
        InvitationStanding::Usable);
  CHECK(invitation_standing::assess(stored("r-expired"), static_cast<int64_t>(std::time(nullptr))).standing ==
        InvitationStanding::Invalid);
  CHECK(invitation_standing::assess(stored("r-spent"), static_cast<int64_t>(std::time(nullptr))).standing ==
        InvitationStanding::Invalid);
  CHECK(invitation_standing::assess(stored("r-manual"), static_cast<int64_t>(std::time(nullptr))).standing ==
        InvitationStanding::Invalid);

  identity_change::setSink(nullptr);
  moduleGate().reset();
}

TEST_CASE("the boot pass revokes the pending invitations of every module found off")
{
  boot();
  seedOwner();
  const int64_t later = static_cast<int64_t>(std::time(nullptr)) + 600;
  insertInvitation({.hash = "b-guard", .role = "guard", .redeemed = 0, .expiresAt = later, .revoked = 0});
  insertInvitation({.hash = "b-resident", .role = "resident", .redeemed = 0, .expiresAt = later, .revoked = 0});

  moduleGate().reset();
  moduleGate().apply({surveillance(true)});
  CHECK(drogon::sync_wait(InvitationModuleRevocation{}.revokeDisabledModules()) == 0);
  CHECK(text("SELECT revoked_reason FROM user_invitation WHERE token_hash = 'b-guard'") == "<null>");

  moduleGate().apply({surveillance(false)});
  CHECK(drogon::sync_wait(InvitationModuleRevocation{}.revokeDisabledModules()) == 1);
  CHECK(text("SELECT revoked_reason FROM user_invitation WHERE token_hash = 'b-guard'") == "module_disabled");
  CHECK(text("SELECT revoked_module FROM user_invitation WHERE token_hash = 'b-guard'") == "surveillance");
  CHECK(text("SELECT revoked_reason FROM user_invitation WHERE token_hash = 'b-resident'") == "<null>");
  moduleGate().reset();
}

TEST_CASE("redeeming an invitation of a role whose module is off is refused, never consumes it, and a later redemption still works")
{
  boot();
  seedOwner();
  const int64_t later = static_cast<int64_t>(std::time(nullptr)) + 600;
  insertInvitation({.hash = "c-guard", .role = "guard", .redeemed = 0, .expiresAt = later, .revoked = 0});

  moduleGate().reset();
  moduleGate().apply({surveillance(false)});
  const auto refused = drogon::sync_wait(redeem("c-guard", UserRole::Guard));
  CHECK(refused.status == RedemptionStatus::ModuleDisabled);
  CHECK(refused.moduleId == "surveillance");
  CHECK(scalar("SELECT redemption_count FROM user_invitation WHERE token_hash = 'c-guard'") == 0);
  CHECK(text("SELECT revoked_at FROM user_invitation WHERE token_hash = 'c-guard'") == "<null>");

  moduleGate().apply({surveillance(true)});
  const auto accepted = drogon::sync_wait(redeem("c-guard", UserRole::Guard));
  CHECK(accepted.status == RedemptionStatus::Consumed);
  CHECK(scalar("SELECT redemption_count FROM user_invitation WHERE token_hash = 'c-guard'") == 1);
  CHECK(drogon::sync_wait(redeem("c-guard", UserRole::Guard)).status == RedemptionStatus::Invalid);
  moduleGate().reset();
}

TEST_CASE("an invitation the module revoked is refused with the module as the reason, and other dead invitations are only invalid")
{
  boot();
  seedOwner();
  const int64_t later = static_cast<int64_t>(std::time(nullptr)) + 600;
  insertInvitation({.hash = "d-guard", .role = "guard", .redeemed = 0, .expiresAt = later, .revoked = 0});
  insertInvitation({.hash = "d-manual", .role = "resident", .redeemed = 0, .expiresAt = later, .revoked = 1});
  insertInvitation({.hash = "d-expired", .role = "resident", .redeemed = 0, .expiresAt = 1, .revoked = 0});

  moduleGate().reset();
  moduleGate().apply({surveillance(false)});
  CHECK(drogon::sync_wait(InvitationModuleRevocation{}.revokeModule("surveillance")) >= 1);
  moduleGate().apply({surveillance(true)});

  const auto revoked = drogon::sync_wait(redeem("d-guard", UserRole::Guard));
  CHECK(revoked.status == RedemptionStatus::ModuleDisabled);
  CHECK(revoked.moduleId == "surveillance");
  CHECK(scalar("SELECT redemption_count FROM user_invitation WHERE token_hash = 'd-guard'") == 0);
  CHECK(drogon::sync_wait(redeem("d-manual", UserRole::Resident)).status == RedemptionStatus::Invalid);
  CHECK(drogon::sync_wait(redeem("d-expired", UserRole::Resident)).status == RedemptionStatus::Invalid);
  CHECK(drogon::sync_wait(redeem("d-unknown", UserRole::Resident)).status == RedemptionStatus::Invalid);
  moduleGate().reset();
}

TEST_CASE("resolving an invitation of a module that is off answers 410 INVITATION_MODULE_DISABLED with the module id")
{
  boot();
  seedOwner();
  const int64_t later = static_cast<int64_t>(std::time(nullptr)) + 600;
  insertInvitation({.hash = "e-guard", .role = "guard", .redeemed = 0, .expiresAt = later, .revoked = 0});
  insertInvitation({.hash = "e-resident", .role = "resident", .redeemed = 0, .expiresAt = later, .revoked = 0});
  const auto now = static_cast<int64_t>(std::time(nullptr));

  moduleGate().reset();
  moduleGate().apply({surveillance(false)});
  try {
    static_cast<void>(invitation_standing::require(stored("e-guard"), now));
    FAIL("a guard invitation must not resolve while surveillance is off");
  }
  catch (const ResponseException& error) {
    CHECK(error.statusCode() == 410);
    CHECK(error.errorCode() == "INVITATION_MODULE_DISABLED");
    const auto* list = std::get_if<std::vector<ResponseError>>(&error.errors());
    REQUIRE(list != nullptr);
    REQUIRE(list->size() == 2);
    CHECK((*list)[1].code == "MODULE_ID");
    CHECK((*list)[1].message == "surveillance");
  }

  CHECK(invitation_standing::require(stored("e-resident"), now).role == UserRole::Resident);
  try {
    static_cast<void>(invitation_standing::require(stored("e-nobody"), now));
    FAIL("an unknown invitation must not resolve");
  }
  catch (const ResponseException& error) {
    CHECK(error.statusCode() == 404);
    CHECK(error.errorCode() == "NOT_FOUND");
  }

  moduleGate().apply({surveillance(true)});
  CHECK(invitation_standing::require(stored("e-guard"), now).role == UserRole::Guard);
  moduleGate().reset();
}

TEST_CASE("a database from before the revocation columns gets them at boot and keeps its invitations readable")
{
  boot();
  seedOwner();
  const int64_t later = static_cast<int64_t>(std::time(nullptr)) + 600;
  insertInvitation({.hash = "f-old", .role = "resident", .redeemed = 0, .expiresAt = later, .revoked = 0});

  const auto client = DbService::client();
  client->execSqlSync("ALTER TABLE user_invitation DROP COLUMN revoked_reason");
  client->execSqlSync("ALTER TABLE user_invitation DROP COLUMN revoked_module");
  const std::string columns =
      "SELECT COUNT(*) FROM pragma_table_info('user_invitation') WHERE name IN ('revoked_reason', 'revoked_module')";
  REQUIRE(scalar(columns) == 0);

  UserInvitationRepository::ensureColumns();
  CHECK(scalar(columns) == 2);
  UserInvitationRepository::ensureColumns();
  CHECK(scalar(columns) == 2);

  const auto old = stored("f-old");
  REQUIRE(old.has_value());
  CHECK_FALSE(old.value_or(UserInvitationSchema{}).revokedReason.has_value());
  CHECK_FALSE(old.value_or(UserInvitationSchema{}).revokedModule.has_value());
  CHECK(old.value_or(UserInvitationSchema{}).toJson()["revokedReason"].isNull());
}
