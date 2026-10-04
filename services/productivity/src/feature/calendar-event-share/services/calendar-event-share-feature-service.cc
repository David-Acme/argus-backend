#include "calendar-event-share-feature-service.hxx"

#include <ctime>
#include <errors/response-exception.hxx>
#include <productivity/productivity-errors.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>
#include <auth/role-access.hxx>
#include <productivity/membership-error.hxx>

#include <vector>

drogon::Task<void> CalendarEventShareFeatureService::emitMembership(
    const EmitMembershipInput& input) const
{
  const SyncOperation operation = input.operation;
  const CalendarEventShareSchema& row = input.row;
  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::CalendarEventShare;
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
    LOG_WARN << "user change sink not installed; drop calendar event share membership emit";
    co_return;
  }
  std::vector<int64_t> recipients{input.ownerId, row.userId};
  co_await sink->emitUsers({.userIds = std::move(recipients),
                            .body = std::move(body),
                            .client = input.client});
  co_return;
}

drogon::Task<CalendarEventShareResult>
CalendarEventShareFeatureService::create(const CreateCalendarEventShareDto& body, int64_t actorId) const
{
  const auto parent = co_await parentRepository_.findById(body.calendarEventId);
  if (!parent || parent->ownerId != actorId)
    co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};
  if (body.userId == parent->ownerId)
    co_return {.error = MembershipError::SelfShare, .row = std::nullopt};

  const auto target = co_await directory_.findById(body.userId);
  if (!target || !target->isActive)
    co_return {.error = MembershipError::UserNotFound, .row = std::nullopt};
  if (!role_access::hasAccess({.role = target->role,
                               .table = TableName::CalendarEvent,
                               .perm = RolePermission::Read}))
    co_return {.error = MembershipError::UserNotAllowed, .row = std::nullopt};

  const auto access = shareAccessFromString(body.access);
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  CalendarEventShareSchema row;
  try {
    if (const auto existing = co_await repository_.findExisting(
            {.parentId = body.calendarEventId,
             .userId = body.userId,
             .client = transaction.get()})) {
      row = co_await repository_.updateAccess(
          {.id = existing->id, .access = access, .client = transaction.get()});
      if (row.id == 0) {
        db_transaction::rollback(transaction);
        co_return {.error = MembershipError::ParentNotFound,
                   .row = std::nullopt};
      }
      const auto* sink = user_change::getProductivitySink();
      if (!sink)
        LOG_WARN << "user change sink not installed; drop calendar event share audit";
      else
        co_await sink->publishAudit({.recordId = row.id,
                                     .tableName = TableName::CalendarEventShare,
                                     .before = existing->toJson(),
                                     .after = row.toJson(),
                                     .userIds = {parent->ownerId, row.userId},
                                     .client = transaction.get()});
    }
    else {
      row = co_await repository_.create({.calendarEventId = body.calendarEventId,
                                         .userId = body.userId,
                                         .access = access,
                                         .client = transaction.get()});
      co_await emitMembership({.operation = SyncOperation::Add,
                               .row = row,
                               .ownerId = parent->ownerId,
                               .client = transaction.get()});
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return {.row = row};
}

drogon::Task<CalendarEventShareResult>
CalendarEventShareFeatureService::update(const UpdateInput& input) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  CalendarEventShareSchema row;
  Json::Value before;
  try {
    const auto existing =
        co_await repository_.findById(input.id, transaction.get());
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};
    }

    const auto parent = co_await parentRepository_.findById(
        existing->calendarEventId, transaction.get());
    if (!parent || parent->ownerId != input.actorId) {
      db_transaction::rollback(transaction);
      co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};
    }
    before = existing->toJson();

    row = co_await repository_.updateAccess(
        {.id = input.id,
         .access = shareAccessFromString(input.body.access),
         .client = transaction.get()});
    if (row.id == 0) {
      db_transaction::rollback(transaction);
      co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};
    }

    const auto* sink = user_change::getProductivitySink();
    if (!sink)
      LOG_WARN << "user change sink not installed; drop calendar event share audit";
    else
      co_await sink->publishAudit({.recordId = row.id,
                                   .tableName = TableName::CalendarEventShare,
                                   .before = std::move(before),
                                   .after = row.toJson(),
                                   .userIds = {parent->ownerId, row.userId},
                                   .client = transaction.get()});

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return {.row = row};
}

drogon::Task<bool> CalendarEventShareFeatureService::remove(int64_t id,
                                                int64_t actorId) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  CalendarEventShareSchema before;
  try {
    const auto existing = co_await repository_.findById(id, transaction.get());
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return false;
    }

    const auto parent = co_await parentRepository_.findById(
        existing->calendarEventId, transaction.get());
    if (!parent || parent->ownerId != actorId) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    before = *existing;

    if (!co_await repository_.remove(id, transaction.get())) {
      db_transaction::rollback(transaction);
      co_return false;
    }

    co_await emitMembership({.operation = SyncOperation::Delete,
                             .row = before,
                             .ownerId = parent->ownerId,
                             .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return true;
}
