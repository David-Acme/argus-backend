#include "user-feature-service.hxx"

#include <config/app-config.hxx>
#include <shared/contracts/identity-change-sink.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/exceptions/response-exception.hxx>

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
} // namespace

drogon::Task<std::vector<UserSchema>>
UserFeatureService::list(int64_t actorId, UserRole actorRole) const
{
  if (actorRole == UserRole::Owner || actorRole == UserRole::Guard)
    co_return co_await repository_.findAll();

  const auto user = co_await repository_.findById(actorId);
  if (!user)
    throw ResponseException({.message = "User not found",
                             .statusCode = 404,
                             .errorCode = AppConfig::ERROR_CODE_NOT_FOUND});
  co_return std::vector<UserSchema>{*user};
}

drogon::Task<UserSchema>
UserFeatureService::update(const UserManagementUpdateInput& input) const
{
  const auto existing = co_await repository_.findById(input.targetUserId);
  if (!existing)
    throw ResponseException({.message = "User not found",
                             .statusCode = 404,
                             .errorCode = AppConfig::ERROR_CODE_NOT_FOUND});

  if (removesLastActiveOwner(*existing, input) &&
      !co_await repository_.hasOtherActiveOwner(existing->id)) {
    throw ResponseException({.message = "At least one active owner is required",
                             .statusCode = 409,
                             .errorCode = AppConfig::ERROR_CODE_CONFLICT});
  }

  const auto updated = co_await repository_.update(
      existing->id,
      {.name = input.body.name,
       .lastName = input.body.lastName,
       .role = input.body.role,
       .isActive = input.body.isActive});
  if (updated.id == 0)
    throw ResponseException({.message = "User not found",
                             .statusCode = 404,
                             .errorCode = AppConfig::ERROR_CODE_NOT_FOUND});

  if (updated.role != existing->role) {
    socketService_.replaceRoleRooms({
        .userId = updated.id,
        .oldRole = existing->role,
        .newRole = updated.role,
    });
    emitAuthContextChanged(updated);
  }

  auto recipients = co_await repository_.findAll();
  std::vector<int64_t> recipientIds{updated.id};
  for (const auto& recipient : recipients) {
    if (recipient.role == UserRole::Owner || recipient.role == UserRole::Guard)
      recipientIds.push_back(recipient.id);
  }
  co_await syncAuditService_.publishUsers({
      .recordId = updated.id,
      .tableName = TableName::User,
      .before = existing->toJson(),
      .after = updated.toJson(),
      .userIds = std::move(recipientIds),
  });

  if (identity_change::getSink()) {
    identity_change::getSink()->publish(
        {.table = "user", .id = updated.id, .deleted = false,
         .row = updated.toJson()});
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
    socketService_.disconnectUser(updated.id, context);
  }
  co_return updated;
}

drogon::Task<void>
UserFeatureService::deactivate(int64_t targetUserId, int64_t actorId) const
{
  const auto existing = co_await repository_.findById(targetUserId);
  if (!existing)
    throw ResponseException({.message = "User not found",
                             .statusCode = 404,
                             .errorCode = AppConfig::ERROR_CODE_NOT_FOUND});
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
  socketService_.emitUser(user.id, body);
}

drogon::Task<void>
UserFeatureService::recordChange(const UserChangeLogInput& input) const
{
  co_await userActionLogService_.record({
      .userId = input.actorId,
      .recordId = input.after.id,
      .tableName = TableName::User,
      .action = input.action,
      .oldData = input.before.toJson(),
      .newData = input.after.toJson(),
      .ipAddress = "",
  });
  co_return;
}
