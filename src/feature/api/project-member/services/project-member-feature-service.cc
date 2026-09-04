#include "project-member-feature-service.hxx"

#include <ctime>
#include <shared/access/role-access.hxx>

void ProjectMemberFeatureService::emitMembership(SyncOperation operation,
                                         const ProjectMemberSchema& row,
                                         int64_t ownerId) const
{
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
  socketService_.emitUsers({ownerId, row.userId}, body);
}

drogon::Task<void> ProjectMemberFeatureService::emitParent(SyncOperation operation,
                                                   int64_t parentId,
                                                   int64_t userId) const
{
  const auto parent = co_await parentRepository_.findById(parentId);
  if (!parent)
    co_return;

  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::Project;
  if (operation == SyncOperation::Delete) {
    // Revoking is not a delete for anyone else, so only the member who lost
    // access is told to drop the row. The id is all the client needs, and the
    // record is not actually deleted, so no timestamp is invented for it.
    Json::Value tombstone;
    tombstone["id"] = parent->id;
    tombstone["deletedAt"] = static_cast<Json::Int64>(std::time(nullptr));
    body.obj = tombstone;
  }
  else {
    body.obj = parent->toJson();
  }
  socketService_.emitUser(userId, body);
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
  // Sharing with someone whose role cannot read the table would be a silent
  // no-op: the row would never reach their device.
  if (!role_access::hasAccess(target->role, TableName::Project,
                              RolePermission::Read))
    co_return {.error = MembershipError::UserNotAllowed, .row = std::nullopt};

  const auto access = shareAccessFromString(body.access);
  // Re-sharing with the same person changes the level instead of colliding
  // with the unique index.
  if (const auto existing = co_await repository_.findExisting(
          body.projectId, body.userId)) {
    const auto row = co_await repository_.updateAccess(existing->id, access);
    co_await syncAuditService_.publishUsers({
        .recordId = row.id,
        .tableName = TableName::ProjectMember,
        .before = existing->toJson(),
        .after = row.toJson(),
        .userIds = {parent->ownerId, row.userId},
    });
    co_return {.row = row};
  }

  const auto row = co_await repository_.create({
      .projectId = body.projectId,
      .userId = body.userId,
      .access = access,
  });
  emitMembership(SyncOperation::Add, row, parent->ownerId);
  co_await emitParent(SyncOperation::Add, body.projectId, body.userId);
  co_return {.row = row};
}

drogon::Task<ProjectMemberResult>
ProjectMemberFeatureService::update(int64_t id, const UpdateProjectMemberDto& body,
                            int64_t actorId) const
{
  const auto existing = co_await repository_.findById(id);
  if (!existing)
    co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};

  const auto parent = co_await parentRepository_.findById(existing->projectId);
  if (!parent || parent->ownerId != actorId)
    co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};

  const auto row = co_await repository_.updateAccess(
      id, shareAccessFromString(body.access));
  if (row.id == 0)
    co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};
  co_await syncAuditService_.publishUsers({
      .recordId = row.id,
      .tableName = TableName::ProjectMember,
      .before = existing->toJson(),
      .after = row.toJson(),
      .userIds = {parent->ownerId, row.userId},
  });
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

  emitMembership(SyncOperation::Delete, *existing, parent->ownerId);
  co_await emitParent(SyncOperation::Delete, existing->projectId,
                      existing->userId);
  co_return true;
}
