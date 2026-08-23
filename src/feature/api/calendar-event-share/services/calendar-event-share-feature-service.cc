#include "calendar-event-share-feature-service.hxx"

#include <shared/access/role-access.hxx>

void CalendarEventShareFeatureService::emitMembership(SyncOperation operation,
                                         const CalendarEventShareSchema& row,
                                         int64_t ownerId) const
{
  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::CalendarEventShare;
  if (operation == SyncOperation::Delete) {
    Json::Value tombstone;
    tombstone["id"] = row.id;
    body.obj = tombstone;
  }
  else {
    body.obj = row.toJson();
  }
  socketService_.emitUsers({ownerId, row.userId}, body);
}

drogon::Task<void> CalendarEventShareFeatureService::emitParent(SyncOperation operation,
                                                   int64_t parentId,
                                                   int64_t userId) const
{
  const auto parent = co_await parentRepository_.findById(parentId);
  if (!parent)
    co_return;

  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::CalendarEvent;
  if (operation == SyncOperation::Delete) {
    // Revoking is not a delete for anyone else, so only the member who lost
    // access is told to drop the row. The id is all the client needs, and the
    // record is not actually deleted, so no timestamp is invented for it.
    Json::Value tombstone;
    tombstone["id"] = parent->id;
    body.obj = tombstone;
  }
  else {
    body.obj = parent->toJson();
  }
  socketService_.emitUser(userId, body);
  co_return;
}

drogon::Task<CalendarEventShareResult>
CalendarEventShareFeatureService::create(const CreateCalendarEventShareDto& body, int64_t actorId) const
{
  const auto parent = co_await parentRepository_.findById(body.calendarEventId);
  if (!parent || parent->ownerId != actorId)
    co_return {.error = MembershipError::ParentNotFound};
  if (body.userId == parent->ownerId)
    co_return {.error = MembershipError::SelfShare};

  const auto target = co_await userRepository_.findById(body.userId);
  if (!target || !target->isActive)
    co_return {.error = MembershipError::UserNotFound};
  // Sharing with someone whose role cannot read the table would be a silent
  // no-op: the row would never reach their device.
  if (!role_access::hasAccess(target->role, TableName::CalendarEvent,
                              RolePermission::Read))
    co_return {.error = MembershipError::UserNotAllowed};

  const auto access = shareAccessFromString(body.access);
  // Re-sharing with the same person changes the level instead of colliding
  // with the unique index.
  if (const auto existing = co_await repository_.findExisting(
          body.calendarEventId, body.userId)) {
    const auto row = co_await repository_.updateAccess(existing->id, access);
    co_await syncAuditService_.publishUsers({
        .recordId = row.id,
        .tableName = TableName::CalendarEventShare,
        .before = existing->toJson(),
        .after = row.toJson(),
        .userIds = {parent->ownerId, row.userId},
    });
    co_return {.row = row};
  }

  const auto row = co_await repository_.create({
      .calendarEventId = body.calendarEventId,
      .userId = body.userId,
      .access = access,
  });
  emitMembership(SyncOperation::Add, row, parent->ownerId);
  co_await emitParent(SyncOperation::Add, body.calendarEventId, body.userId);
  co_return {.row = row};
}

drogon::Task<CalendarEventShareResult>
CalendarEventShareFeatureService::update(int64_t id, const UpdateCalendarEventShareDto& body,
                            int64_t actorId) const
{
  const auto existing = co_await repository_.findById(id);
  if (!existing)
    co_return {.error = MembershipError::ParentNotFound};

  const auto parent = co_await parentRepository_.findById(existing->calendarEventId);
  if (!parent || parent->ownerId != actorId)
    co_return {.error = MembershipError::ParentNotFound};

  const auto row = co_await repository_.updateAccess(
      id, shareAccessFromString(body.access));
  if (row.id == 0)
    co_return {.error = MembershipError::ParentNotFound};
  co_await syncAuditService_.publishUsers({
      .recordId = row.id,
      .tableName = TableName::CalendarEventShare,
      .before = existing->toJson(),
      .after = row.toJson(),
      .userIds = {parent->ownerId, row.userId},
  });
  co_return {.row = row};
}

drogon::Task<bool> CalendarEventShareFeatureService::remove(int64_t id,
                                                int64_t actorId) const
{
  const auto existing = co_await repository_.findById(id);
  if (!existing)
    co_return false;

  const auto parent = co_await parentRepository_.findById(existing->calendarEventId);
  if (!parent || parent->ownerId != actorId)
    co_return false;

  const bool removed = co_await repository_.remove(id);
  if (!removed)
    co_return false;

  emitMembership(SyncOperation::Delete, *existing, parent->ownerId);
  co_await emitParent(SyncOperation::Delete, existing->calendarEventId,
                      existing->userId);
  co_return true;
}
