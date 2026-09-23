#include "user-feature-service.hxx"

#include <auth/user-role.hxx>
#include <errors/response-exception.hxx>
#include <identity/identity-errors.hxx>
#include <sync/identity-change-sink.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-control-sink.hxx>
#include <sync/sync-operation.hxx>
#include <trantor/utils/Logger.h>

namespace
{
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
  if (actorRole == UserRole::Owner || actorRole == UserRole::Guard)
    co_return co_await repository_.findAll();

  const auto user = co_await repository_.findById(actorId);
  if (!user)
    throw ResponseException(404, IdentityErrors::UserNotFound);
  co_return std::vector<UserSchema>{*user};
}

drogon::Task<UserSchema>
UserFeatureService::update(const UserManagementUpdateInput& input) const
{
  const auto existing = co_await repository_.findById(input.targetUserId);
  if (!existing)
    throw ResponseException(404, IdentityErrors::UserNotFound);

  if (removesLastActiveOwner(*existing, input) &&
      !co_await repository_.hasOtherActiveOwner(existing->id)) {
    throw ResponseException(409, IdentityErrors::ActiveOwnerRequired);
  }

  const auto updated = co_await repository_.update(
      existing->id,
      {.name = input.body.name,
       .lastName = input.body.lastName,
       .role = input.body.role,
       .isActive = input.body.isActive});
  if (updated.id == 0)
    throw ResponseException(404, IdentityErrors::UserNotFound);

  if (updated.role != existing->role) {
    if (const auto* control = sync_control::getSink()) {
      const bool replaced = control->replaceRoleRooms(
          {.userId = updated.id,
           .oldRole = userRoleToString(existing->role),
           .newRole = userRoleToString(updated.role)});
      if (!replaced)
        LOG_WARN << "Identity: role room replace failed for user "
                 << updated.id;
    }
    emitAuthContextChanged(updated);
  }

  auto recipients = co_await repository_.findAll();
  std::vector<int64_t> recipientIds{updated.id};
  for (const auto& recipient : recipients) {
    if (recipient.role == UserRole::Owner || recipient.role == UserRole::Guard)
      recipientIds.push_back(recipient.id);
  }
  if (const auto* sink = identity_change::getSink()) {
    co_await sink->publishUsersAudit({
        .recordId = updated.id,
        .tableName = TableName::User,
        .before = existing->toJson(),
        .after = updated.toJson(),
        .userIds = std::move(recipientIds),
    });
  }

  if (const auto* sink = identity_change::getSink()) {
    const IdentityCatalogInput catalog{.table = TableName::User,
                                       .id = updated.id,
                                       .deleted = false,
                                       .row = updated.toJson()};
    co_await sink->publishCatalog(catalog);
  }
  co_await recordChange({
      .actorId = input.actorId,
      .before = *existing,
      .after = updated,
      .action = UserAction::Update,
  });

  if (existing->isActive && !updated.isActive) {
    co_await refreshTokenRepository_.invalidateAllUser(updated.id);
    SocketEmitDto context;
    context.operation = SyncOperation::AuthContextChanged;
    context.option = TableName::User;
    context.obj = updated.toJson();
    context.obj["resync"] = false;
    if (const auto* control = sync_control::getSink()) {
      const bool disconnected = control->disconnectUser(updated.id, context);
      if (!disconnected)
        LOG_WARN << "Identity: disconnect failed for user " << updated.id;
    }
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
    co_await sink->publishAction({
        .userId = input.actorId,
        .recordId = input.after.id,
        .tableName = TableName::User,
        .action = input.action,
        .oldData = input.before.toJson(),
        .newData = input.after.toJson(),
        .ipAddress = "",
    });
  }
  co_return;
}
