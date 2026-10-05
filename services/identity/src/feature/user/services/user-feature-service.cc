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


namespace
{
BlockingStrand& sessionNotices()
{
  static BlockingStrand strand;
  return strand;
}

bool removesLastActiveOwner(const UserSchema& before,
                            const UserManagementUpdateInput& input)
{
  if (before.role != UserRole::Owner || !before.isActive)
    return false;
  const auto nextRole = input.body.userRole.value_or(before.role);
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
  if (input.targetUserId == input.actorId && input.body.isActive.has_value() &&
      !*input.body.isActive)
    throw ResponseException(IdentityErrors::SelfDeactivationForbidden);

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

    const bool guardsLastOwner = removesLastActiveOwner(*existing, input);
    if (guardsLastOwner &&
        !co_await repository_.hasOtherActiveOwner(existing->id,
                                                  transaction.get())) {
      throw ResponseException(409, IdentityErrors::ActiveOwnerRequired);
    }

    updated = co_await repository_.update(
        existing->id,
        {.name = input.body.name,
         .lastName = input.body.lastName,
         .role = input.body.userRole,
         .isActive = input.body.isActive,
         .requireOtherActiveOwner = guardsLastOwner,
         .client = transaction.get()});
    if (updated.id == 0 && guardsLastOwner)
      throw ResponseException(409, IdentityErrors::ActiveOwnerRequired);
    if (updated.id == 0)
      throw ResponseException(404, IdentityErrors::UserNotFound);

    roleChanged = updated.role != existing->role;
    deactivated = existing->isActive && !updated.isActive;

    co_await publishDirectoryAudit(
        {.before = *existing, .after = updated, .client = transaction.get()});

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
    co_await BlockingTask<void>(
        [this, updated, oldRole = existing->role, roleChanged, deactivated] {
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
        },
        sessionNotices());
  }
  co_return updated;
}

drogon::Task<void>
UserFeatureService::deactivate(int64_t targetUserId, int64_t actorId) const
{
  const auto existing = co_await repository_.findById(targetUserId);
  if (!existing)
    throw ResponseException(404, IdentityErrors::UserNotFound);
  if (targetUserId == actorId)
    throw ResponseException(IdentityErrors::SelfDeactivationForbidden);
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

drogon::Task<void>
UserFeatureService::publishDirectoryAudit(const DirectoryAuditInput& input) const
{
  const auto* sink = identity_change::getSink();
  if (sink == nullptr)
    co_return;
  std::vector<int64_t> recipientIds{input.after.id};
  for (const auto& recipient : co_await repository_.findAll(input.client)) {
    if (recipient.id != input.after.id &&
        role_access::readsUserDirectory(recipient.role))
      recipientIds.push_back(recipient.id);
  }
  co_await sink->publishUsersAudit({
      .recordId = input.after.id,
      .tableName = TableName::User,
      .before = input.before.toJson(),
      .after = input.after.toJson(),
      .userIds = std::move(recipientIds),
      .client = input.client,
  });
}

drogon::Task<std::optional<UserSchema>>
UserFeatureService::rename(const UserRenameInput& input) const
{
  std::optional<UserSchema> renamed;
  auto transaction =
      co_await db_transaction::begin(DbService::identityClient());
  try {
    const auto before = co_await repository_.findById(input.userId,
                                                      transaction.get());
    if (!before) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }
    const auto user = co_await repository_.update(
        input.userId, {.name = input.name,
                       .lastName = std::nullopt,
                       .role = std::nullopt,
                       .isActive = std::nullopt,
                       .requireOtherActiveOwner = false,
                       .client = transaction.get()});
    if (user.id <= 0) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }
    co_await publishDirectoryAudit(
        {.before = *before, .after = user, .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
    renamed = user;
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return renamed;
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
