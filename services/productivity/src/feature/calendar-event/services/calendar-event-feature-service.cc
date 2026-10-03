#include "calendar-event-feature-service.hxx"

#include <ctime>
#include <errors/response-exception.hxx>
#include <productivity/productivity-errors.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>

drogon::Task<void>
CalendarEventFeatureService::emit(const EmitInput& input) const
{
  const SyncOperation operation = input.operation;
  const CalendarEventSchema& row = input.row;
  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::CalendarEvent;
  if (operation == SyncOperation::Delete) {
    Json::Value tombstone;
    tombstone["id"] = row.id;
    tombstone["deletedAt"] = row.deletedAt.value_or(std::time(nullptr));
    body.obj = tombstone;
  }
  else {
    body.obj = row.toJson();
  }

  auto recipients = co_await shareRepository_.memberIds(row.id, input.client);
  recipients.push_back(row.ownerId);
  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop calendar event emit";
    co_return;
  }
  co_await sink->emitUsers({.userIds = std::move(recipients),
                            .body = std::move(body),
                            .client = input.client});
  co_return;
}

drogon::Task<bool>
CalendarEventFeatureService::canEdit(const CanEditInput& input) const
{
  if (input.row.ownerId == input.actorId)
    co_return true;
  const auto access = co_await shareRepository_.findAccess(
      {.parentId = input.row.id, .userId = input.actorId, .client = input.client});
  co_return access && *access == ShareAccess::Edit;
}

drogon::Task<CalendarEventSchema>
CalendarEventFeatureService::create(const CreateCalendarEventDto& body,
                                    const CalendarEventOwnerInput& who) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  CalendarEventSchema row;
  try {
    row = co_await repository_.create({
        .createdBy = who.actorId > 0 ? std::optional<int64_t>(who.actorId)
                                     : std::nullopt,
        .ownerId = who.ownerId,
        .projectId = body.projectId,
        .title = body.title,
        .description = body.description,
        .location = body.location,
        .color = body.color,
        .startsAt = body.startsAt,
        .endsAt = body.endsAt,
        .isAllDay = body.isAllDay,
        .recurrenceRule = body.recurrenceRule,
        .client = transaction.get(),
    });
    co_await emit({.operation = SyncOperation::Add,
                   .row = row,
                   .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return row;
}

drogon::Task<std::optional<CalendarEventSchema>>
CalendarEventFeatureService::update(const UpdateInput& input) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  CalendarEventSchema row;
  Json::Value before;
  try {
    const auto existing =
        co_await repository_.findById(input.id, transaction.get());
    if (!existing ||
        !co_await canEdit({.row = *existing,
                           .actorId = input.actorId,
                           .client = transaction.get()})) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }
    before = existing->toJson();

    row = co_await repository_.update(input.id, {
        .title = input.body.title,
        .description = input.body.description,
        .location = input.body.location,
        .color = input.body.color,
        .startsAt = input.body.startsAt,
        .endsAt = input.body.endsAt,
        .clearEndsAt = input.body.clearsEndsAt,
        .isAllDay = input.body.isAllDay,
        .recurrenceRule = input.body.recurrenceRule,
        .projectId = input.body.projectId,
        .client = transaction.get(),
    });
    if (row.id == 0) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }

    auto recipients =
        co_await shareRepository_.memberIds(row.id, transaction.get());
    recipients.push_back(row.ownerId);
    const auto* sink = user_change::getProductivitySink();
    if (!sink)
      LOG_WARN << "user change sink not installed; drop calendar event audit";
    else
      co_await sink->publishAudit({.recordId = row.id,
                                   .tableName = TableName::CalendarEvent,
                                   .before = std::move(before),
                                   .after = row.toJson(),
                                   .userIds = std::move(recipients),
                                   .client = transaction.get()});

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return row;
}

drogon::Task<bool> CalendarEventFeatureService::remove(int64_t id,
                                                       int64_t actorId) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  CalendarEventSchema before;
  try {
    const auto existing = co_await repository_.findById(id, transaction.get());
    if (!existing || existing->ownerId != actorId) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    before = *existing;

    if (!co_await repository_.remove(id, transaction.get())) {
      db_transaction::rollback(transaction);
      co_return false;
    }

    co_await emit({.operation = SyncOperation::Delete,
                   .row = before,
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
