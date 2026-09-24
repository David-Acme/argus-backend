#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/invitation/services/invitation-feature-service.hxx>
#include <feature/user/services/nats-identity-change-sink.hxx>
#include <feature/user/services/user-feature-service.hxx>
#include <nats/nats-subject.hxx>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
#include <sqlite/db-service.hxx>
#include <sync/identity-change-sink.hxx>
#include <sync/module-audit-event.hxx>
#include <sync/module-emit.hxx>
#include <sync/user-audit-event.hxx>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kTransactionDb = "identity-change-transaction-test.db";
constexpr int64_t kUserId = 7;

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

class RefusingSink : public IdentityChangeSink
{
public:
  explicit RefusingSink(const drogon::orm::DbClient* pooled) : pooled_(pooled)
  {
  }

  [[nodiscard]] drogon::Task<void>
  publishCatalog(const IdentityCatalogInput& input) const override
  {
    note(input.client);
    if (refuse_)
      throw std::runtime_error("the change sink refused the catalog row");
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  emitModule(const ModuleEmitInput& input) const override
  {
    note(input.client);
    if (refuse_)
      throw std::runtime_error("the change sink refused the emit");
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishModuleAudit(const ModuleAuditInput& input) const override
  {
    note(input.client);
    if (refuse_)
      throw std::runtime_error("the change sink refused the module audit");
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishUsersAudit(const UserAuditInput& input) const override
  {
    note(input.client);
    if (refuse_)
      throw std::runtime_error("the change sink refused the user audit");
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAction(const ActionPublishInput& input) const override
  {
    note(input.client);
    if (refuse_)
      throw std::runtime_error("the change sink refused the action");
    co_return;
  }

  void refuse(bool value) const { refuse_ = value; }
  [[nodiscard]] int calls() const { return calls_; }
  [[nodiscard]] bool sawClient() const { return sawClient_; }
  [[nodiscard]] bool transactional() const { return transactional_; }

private:
  void note(const drogon::orm::DbClient* client) const
  {
    ++calls_;
    sawClient_ = client != nullptr;
    transactional_ = sawClient_ && client != pooled_;
  }

  mutable bool refuse_{false};
  mutable int calls_{0};
  mutable bool sawClient_{false};
  mutable bool transactional_{false};
  const drogon::orm::DbClient* pooled_;
};

CreateInvitationDto invitationBody(int64_t expiresAt)
{
  CreateInvitationDto dto;
  dto.role = UserRole::Resident;
  dto.roleValue = userRoleToString(UserRole::Resident);
  dto.maxRedemptions = 1;
  dto.expiresAt = expiresAt;
  return dto;
}

void seedUser(int64_t userId)
{
  DbService::client()->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active) "
      "VALUES (?, 'Ana', '', 'guest', 'es', 1)",
      userId);
}

int64_t liveInvitations()
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT COUNT(*) AS total FROM user_invitation");
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}

bool invitationRevoked(int64_t invitationId)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT revoked_at FROM user_invitation WHERE id = ?", invitationId);
  return !rows.empty() && !rows.front()["revoked_at"].isNull();
}

std::string storedName(int64_t userId)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT name FROM user WHERE id = ?", userId);
  return rows.empty() ? std::string{} : rows.front()["name"].as<std::string>();
}

std::string storedRole(int64_t userId)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT role FROM user WHERE id = ?", userId);
  return rows.empty() ? std::string{} : rows.front()["role"].as<std::string>();
}

std::size_t countSubject(const std::vector<ChangeOutboxRow>& rows,
                         const std::string& subject)
{
  std::size_t total = 0;
  for (const auto& row : rows) {
    if (row.subject == subject)
      ++total;
  }
  return total;
}

std::size_t drainOutbox(const ChangeOutboxRepository& outbox)
{
  std::size_t sent = 0;
  for (const auto& row : outbox.pendingBatch(64)) {
    if (outbox.markSent(row.id, 1000 + static_cast<int64_t>(sent)))
      ++sent;
  }
  return sent;
}
}

TEST_CASE("an identity write and its change are one unit of work")
{
  std::remove(kTransactionDb);
  std::remove((std::string(kTransactionDb) + "-wal").c_str());
  std::remove((std::string(kTransactionDb) + "-shm").c_str());
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                 .filename = kTransactionDb,
                                 .name = "default",
                                 .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));

  InvitationFeatureService invitations;
  UserFeatureService users;
  ChangeOutboxRepository outbox;
  const auto pooled = DbService::client();

  RefusingSink sink(pooled.get());
  identity_change::setSink(&sink);

  const int64_t expiresAt = std::time(nullptr) + 3600;
  seedUser(kUserId);

  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(invitations.create(
                      invitationBody(expiresAt), kUserId)),
                  std::runtime_error);
  CHECK(liveInvitations() == 0);

  sink.refuse(false);
  const int beforeCreate = sink.calls();
  const auto created =
      drogon::sync_wait(invitations.create(invitationBody(expiresAt), kUserId));
  CHECK(created.invitation.id > 0);
  CHECK(created.token.size() == 43);
  CHECK(sink.calls() == beforeCreate + 2);
  CHECK(sink.sawClient());
  CHECK(sink.transactional());
  CHECK(liveInvitations() == 1);

  UpdateUserDto promote;
  promote.role = UserRole::Resident;

  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(users.update({.targetUserId = kUserId,
                                                  .actorId = kUserId,
                                                  .body = promote})),
                  std::runtime_error);
  CHECK(storedRole(kUserId) == "guest");

  sink.refuse(false);
  const auto promoted = drogon::sync_wait(users.update({.targetUserId = kUserId,
                                                        .actorId = kUserId,
                                                        .body = promote}));
  CHECK(promoted.id == kUserId);
  CHECK(storedRole(kUserId) == "resident");
  CHECK(sink.transactional());

  sink.refuse(true);
  CHECK_THROWS_AS(
      drogon::sync_wait(invitations.revoke(created.invitation.id, kUserId)),
      std::runtime_error);
  CHECK_FALSE(invitationRevoked(created.invitation.id));

  sink.refuse(false);
  drogon::sync_wait(invitations.revoke(created.invitation.id, kUserId));
  CHECK(invitationRevoked(created.invitation.id));
  CHECK(sink.transactional());

  CHECK(drainOutbox(outbox) == 0);

  NatsIdentityChangeSink durableSink(
      nullptr,
      NatsIdentityChangeSink::Config{.retryMs = 20,
                                     .changeSubject = {},
                                     .actionSubject = {},
                                     .streamName = {}});
  identity_change::setSink(&durableSink);

  const auto durable =
      drogon::sync_wait(invitations.create(invitationBody(expiresAt), kUserId));
  CHECK(durable.invitation.id > 0);
  {
    const auto pending = outbox.pendingBatch(10);
    REQUIRE(pending.size() == 2);
    CHECK(countSubject(pending, nats_subject::kIdentityChange) == 1);
    CHECK(countSubject(pending, nats_subject::kIdentityUserAction) == 1);
    const std::string invitationId =
        std::to_string(durable.invitation.id);
    for (const auto& row : pending)
      CHECK(row.payload.find(invitationId) != std::string::npos);
  }
  CHECK(drainOutbox(outbox) == 2);

  UpdateUserDto rename;
  rename.name = "Ana Maria";
  const auto renamed = drogon::sync_wait(users.update({.targetUserId = kUserId,
                                                       .actorId = kUserId,
                                                       .body = rename}));
  CHECK(renamed.name == "Ana Maria");
  {
    const auto pending = outbox.pendingBatch(10);
    REQUIRE(pending.size() == 3);
    CHECK(countSubject(pending, nats_subject::kIdentityChange) == 2);
    CHECK(countSubject(pending, nats_subject::kIdentityUserAction) == 1);
    CHECK(pending.front().payload.find("\"current\":\"Ana Maria\"") !=
          std::string::npos);
  }
  CHECK(drainOutbox(outbox) == 3);

  DbService::client()->execSqlSync("DROP TABLE change_outbox");

  const int64_t invitationsBefore = liveInvitations();
  CHECK_THROWS(
      drogon::sync_wait(invitations.create(invitationBody(expiresAt), kUserId)));
  CHECK(liveInvitations() == invitationsBefore);

  UpdateUserDto orphan;
  orphan.name = "Orphan";
  CHECK_THROWS(drogon::sync_wait(users.update({.targetUserId = kUserId,
                                               .actorId = kUserId,
                                               .body = orphan})));
  CHECK(storedName(kUserId) == "Ana Maria");

  CHECK_THROWS(drogon::sync_wait(
      invitations.revoke(durable.invitation.id, kUserId)));
  CHECK_FALSE(invitationRevoked(durable.invitation.id));

  identity_change::setSink(nullptr);
  std::remove(kTransactionDb);
  std::remove((std::string(kTransactionDb) + "-wal").c_str());
  std::remove((std::string(kTransactionDb) + "-shm").c_str());
}
