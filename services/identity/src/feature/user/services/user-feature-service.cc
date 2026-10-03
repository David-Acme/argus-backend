#include "user-feature-service.hxx"

#include <runtime/blocking-task.hxx>
#include <auth/role-access.hxx>
#include <auth/user-role.hxx>
#include <errors/response-exception.hxx>
#include <identity/identity-errors.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-control-sink.hxx>
#include <sync/sync-operation.hxx>
#include <trantor/utils/Logger.h>

#include <condition_variable>
#include <functional>
#include <mutex>

namespace
{
class SessionNoticeOrder
{
public:
  static SessionNoticeOrder& instance()
  {
    static SessionNoticeOrder order;
    return order;
  }

  uint64_t take()
  {
    std::scoped_lock lock(mutex_);
    return next_++;
  }

  void runInTurn(uint64_t ticket, const std::function<void()>& work)
  {
    {
      std::unique_lock lock(mutex_);
      turn_.wait(lock, [this, ticket] { return serving_ == ticket; });
    }
    struct Advance
    {
      SessionNoticeOrder& order;
      ~Advance()
      {
        {
          std::scoped_lock lock(order.mutex_);
          ++order.serving_;
        }
        order.turn_.notify_all();
      }
    } advance{*this};
    work();
  }

private:
  std::mutex mutex_;
  std::condition_variable turn_;
  uint64_t next_{0};
  uint64_t serving_{0};
};

bool removesLastActiveOwner(const UserSchema& before,
                            const UserManagementUpdateInput& input)
{
  if (before.role != UserRole::Owner || !before.isActive)
    return false;
  const auto nextRole = input.body.role.value_or(before.role);
  const auto nextActive = input.body.isActive.value_or(before.isActive);
  return nextRole != UserRole::Owner || !nextActive;
}
}

drogon::Task<std::vector<UserSchema>>
UserFeatureService::list(int64_t actorId, UserRole actorRole) const
{
  if (role_access::readsUserDirectory(actorRole))
    co_return co_await repository_.findAll();

  const auto user = co_await repository_.findById(actorId);
  if (!user)
    throw ResponseException(404, IdentityErrors::UserNotFound);
  co_return std::vector<UserSchema>{*user};
}

drogon::Task<UserSchema>
UserFeatureService::update(const UserManagementUpdateInput& input) const
{
  UserSchema updated;
  std::optional<UserSchema> existing;
  bool roleChanged = false;
  bool deactivated = false;

  auto transaction =
      co_await db_transaction::begin(DbService::identityClient());
  try {
    existing = co_await repository_.findById(input.targetUserId,
                                             transaction.get());
    if (!existing)
      throw ResponseException(404, IdentityErrors::UserNotFound);

    if (removesLastActiveOwner(*existing, input) &&
        !co_await repository_.hasOtherActiveOwner(existing->id,
                                                  transaction.get())) {
      throw ResponseException(409, IdentityErrors::ActiveOwnerRequired);
    }

    updated = co_await repository_.update(
        existing->id,
        {.name = input.body.name,
         .lastName = input.body.lastName,
         .role = input.body.role,
         .isActive = input.body.isActive,
         .client = transaction.get()});
    if (updated.id == 0)
      throw ResponseException(404, IdentityErrors::UserNotFound);

    roleChanged = updated.role != existing->role;
    deactivated = existing->isActive && !updated.isActive;

    auto recipients = co_await repository_.findAll(transaction.get());
    std::vector<int64_t> recipientIds{updated.id};
    for (const auto& recipient : recipients) {
      if (recipient.id != updated.id &&
          role_access::readsUserDirectory(recipient.role))
        recipientIds.push_back(recipient.id);
    }
    if (const auto* sink = identity_change::getSink()) {
      co_await sink->publishUsersAudit({
          .recordId = updated.id,
          .tableName = TableName::User,
          .before = existing->toJson(),
          .after = updated.toJson(),
          .userIds = std::move(recipientIds),
          .client = transaction.get(),
      });
    }

    if (const auto* sink = identity_change::getSink()) {
      const IdentityCatalogInput catalog{.table = TableName::User,
                                         .id = updated.id,
                                         .deleted = false,
                                         .row = updated.toJson(),
                                         .client = transaction.get()};
      co_await sink->publishCatalog(catalog);
    }
    co_await recordChange({
        .actorId = input.actorId,
        .before = *existing,
        .after = updated,
        .action = UserAction::Update,
        .client = transaction.get(),
    });

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  if (roleChanged || deactivated) {
    const uint64_t ticket = SessionNoticeOrder::instance().take();
    co_await BlockingTask<void>([this, updated, oldRole = existing->role,
                                 roleChanged, deactivated, ticket] {
      SessionNoticeOrder::instance().runInTurn(ticket, [&] {
        const auto* control = sync_control::getSink();
        if (roleChanged) {
          if (control &&
              !control->replaceRoleRooms({.userId = updated.id,
                                          .oldRole = userRoleToString(oldRole),
                                          .newRole = userRoleToString(updated.role)}))
            LOG_WARN << "Identity: role room replace failed for user "
                     << updated.id;
          emitAuthContextChanged(updated);
        }
        if (deactivated && control) {
          SocketEmitDto context;
          context.operation = SyncOperation::AuthContextChanged;
          context.option = TableName::User;
          context.obj = updated.toJson();
          context.obj["resync"] = false;
          if (!control->disconnectUser(updated.id, context))
            LOG_WARN << "Identity: disconnect failed for user " << updated.id;
        }
      });
    });
  }
  co_return updated;
}

drogon::Task<void>
UserFeatureService::deactivate(int64_t targetUserId, int64_t actorId) const
{
  const auto existing = co_await repository_.findById(targetUserId);
  if (!existing)
    throw ResponseException(404, IdentityErrors::UserNotFound);
  if (!existing->isActive)
    co_return;

  UpdateUserDto body;
  body.isActive = false;
  co_await update({
      .targetUserId = targetUserId,
      .actorId = actorId,
      .body = body,
  });
  co_return;
}

void UserFeatureService::emitAuthContextChanged(const UserSchema& user) const
{
  SocketEmitDto body;
  body.operation = SyncOperation::AuthContextChanged;
  body.option = TableName::User;
  body.obj = user.toJson();
  body.obj["resync"] = true;
  if (const auto* control = sync_control::getSink()) {
    const bool emitted = control->emitToUser(user.id, body);
    if (!emitted)
      LOG_WARN << "Identity: auth context emit failed for user " << user.id;
  }
}

drogon::Task<void>
UserFeatureService::recordChange(const UserChangeLogInput& input) const
{
  if (const auto* sink = identity_change::getSink()) {
    co_await sink->publishAction(
        {.event = {.userId = input.actorId,
                   .recordId = input.after.id,
                   .tableName = TableName::User,
                   .action = input.action,
                   .oldData = input.before.toJson(),
                   .newData = input.after.toJson(),
                   .ipAddress = ""},
         .client = input.client});
  }
  co_return;
}
