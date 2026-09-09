#include "project-member-feature-service.hxx"

#include <ctime>
#include <trantor/utils/Logger.h>
#include <shared/access/role-access.hxx>

void ProjectMemberFeatureService::emitMembership(
    const EmitMembershipInput& input) const
{
  const SyncOperation operation = input.operation;
  const ProjectMemberSchema& row = input.row;
  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::ProjectMember;
  if (operation == SyncOperation::Delete) {
    Json::Value tombstone;
    tombstone["id"] = row.id;
    tombstone["deletedAt"] = row.deletedAt.value_or(std::time(nullptr));
    body.obj = tombstone;
  }
  else {
    body.obj = row.toJson();
  }
  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop project member membership emit";
    return;
  }
  sink->emitUsers({input.ownerId, row.userId}, body);
}

drogon::Task<void> ProjectMemberFeatureService::emitParent(
    const EmitParentInput& input) const
{
  const SyncOperation operation = input.operation;
  const auto parent = co_await parentRepository_.findById(input.parentId);
  if (!parent)
    co_return;

  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::Project;
  if (operation == SyncOperation::Delete) {
    Json::Value tombstone;
    tombstone["id"] = parent->id;
    tombstone["deletedAt"] = static_cast<Json::Int64>(std::time(nullptr));
    body.obj = tombstone;
  }
  else {
    body.obj = parent->toJson();
  }
  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop project member parent emit";
    co_return;
  }
  sink->emitUser(input.userId, body);
  co_return;
}

drogon::Task<ProjectMemberResult>
ProjectMemberFeatureService::create(const CreateProjectMemberDto& body, int64_t actorId) const
{
  const auto parent = co_await parentRepository_.findById(body.projectId);
  if (!parent || parent->ownerId != actorId)
    co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};
  if (body.userId == parent->ownerId)
    co_return {.error = MembershipError::SelfShare, .row = std::nullopt};

  const auto target = co_await userRepository_.findById(body.userId);
  if (!target || !target->isActive)
    co_return {.error = MembershipError::UserNotFound, .row = std::nullopt};
  if (!role_access::hasAccess({.role = target->role,
                               .table = TableName::Project,
                               .perm = RolePermission::Read}))
    co_return {.error = MembershipError::UserNotAllowed, .row = std::nullopt};

  const auto access = shareAccessFromString(body.access);
  if (const auto existing = co_await repository_.findExisting(
          body.projectId, body.userId)) {
    const auto row = co_await repository_.updateAccess(existing->id, access);
    if (const auto* sink = user_change::getProductivitySink())
      co_await sink->publishAudit(UserAuditInput{
          .recordId = row.id,
          .tableName = TableName::ProjectMember,
          .before = existing->toJson(),
          .after = row.toJson(),
          .userIds = {parent->ownerId, row.userId},
      });
    else
      LOG_WARN << "user change sink not installed; drop project member audit";
    co_return {.row = row};
  }

  const auto row = co_await repository_.create({
      .projectId = body.projectId,
      .userId = body.userId,
      .access = access,
  });
  emitMembership({.operation = SyncOperation::Add,
                  .row = row,
                  .ownerId = parent->ownerId});
  co_await emitParent({.operation = SyncOperation::Add,
                       .parentId = body.projectId,
                       .userId = body.userId});
  co_return {.row = row};
}

drogon::Task<ProjectMemberResult>
ProjectMemberFeatureService::update(const UpdateInput& input) const
{
  const auto existing = co_await repository_.findById(input.id);
  if (!existing)
    co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};

  const auto parent = co_await parentRepository_.findById(existing->projectId);
  if (!parent || parent->ownerId != input.actorId)
    co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};

  const auto row = co_await repository_.updateAccess(
      input.id, shareAccessFromString(input.body.access));
  if (row.id == 0)
    co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};
  if (const auto* sink = user_change::getProductivitySink())
    co_await sink->publishAudit(UserAuditInput{
        .recordId = row.id,
        .tableName = TableName::ProjectMember,
        .before = existing->toJson(),
        .after = row.toJson(),
        .userIds = {parent->ownerId, row.userId},
    });
  else
    LOG_WARN << "user change sink not installed; drop project member audit";
  co_return {.row = row};
}

drogon::Task<bool> ProjectMemberFeatureService::remove(int64_t id,
                                                int64_t actorId) const
{
  const auto existing = co_await repository_.findById(id);
  if (!existing)
    co_return false;

  const auto parent = co_await parentRepository_.findById(existing->projectId);
  if (!parent || parent->ownerId != actorId)
    co_return false;

  const bool removed = co_await repository_.remove(id);
  if (!removed)
    co_return false;

  emitMembership({.operation = SyncOperation::Delete,
                  .row = *existing,
                  .ownerId = parent->ownerId});
  co_await emitParent({.operation = SyncOperation::Delete,
                       .parentId = existing->projectId,
                       .userId = existing->userId});
  co_return true;
}
