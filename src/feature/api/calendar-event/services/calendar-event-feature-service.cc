#include "calendar-event-feature-service.hxx"

#include <ctime>
#include <trantor/utils/Logger.h>

drogon::Task<void>
CalendarEventFeatureService::emit(SyncOperation operation,
                                 const CalendarEventSchema& row) const
{
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

  auto recipients = co_await shareRepository_.memberIds(row.id);
  recipients.push_back(row.ownerId);
  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop calendar event emit";
    co_return;
  }
  sink->emitUsers(recipients, body);
  co_return;
}

drogon::Task<bool>
CalendarEventFeatureService::canEdit(const CalendarEventSchema& row,
                                    int64_t actorId) const
{
  if (row.ownerId == actorId)
    co_return true;
  const auto access = co_await shareRepository_.findAccess(row.id, actorId);
  co_return access && *access == ShareAccess::Edit;
}

drogon::Task<CalendarEventSchema>
CalendarEventFeatureService::create(const CreateCalendarEventDto& body,
                                    const CalendarEventOwnerInput& who) const
{
  const auto row = co_await repository_.create({
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
  });
  co_await emit(SyncOperation::Add, row);
  co_return row;
}

drogon::Task<std::optional<CalendarEventSchema>>
CalendarEventFeatureService::update(int64_t id,
                                    const UpdateCalendarEventDto& body,
                                    int64_t actorId) const
{
  const auto existing = co_await repository_.findById(id);
  // The role filter cannot express ownership, so it is checked here: the owner
  // always, a member only when their share grants `edit`.
  if (!existing || !co_await canEdit(*existing, actorId))
    co_return std::nullopt;

  const auto row = co_await repository_.update(id, {
      .title = body.title,
      .description = body.description,
      .location = body.location,
      .color = body.color,
      .startsAt = body.startsAt,
      .endsAt = body.endsAt,
      .isAllDay = body.isAllDay,
      .recurrenceRule = body.recurrenceRule,
      .projectId = body.projectId,
  });
  if (row.id == 0)
    co_return std::nullopt;
  auto recipients = co_await shareRepository_.memberIds(row.id);
  recipients.push_back(row.ownerId);
  const auto* sink = user_change::getProductivitySink();
  if (sink)
    co_await sink->publishAudit(UserAuditInput{
        .recordId = row.id,
        .tableName = TableName::CalendarEvent,
        .before = existing->toJson(),
        .after = row.toJson(),
        .userIds = std::move(recipients),
    });
  else
    LOG_WARN << "user change sink not installed; drop calendar event audit";
  co_return row;
}

drogon::Task<bool> CalendarEventFeatureService::remove(int64_t id,
                                                       int64_t actorId) const
{
  const auto existing = co_await repository_.findById(id);
  // Deleting is the owner's alone: an `edit` member changes the event, it does
  // not get to make it disappear from the owner's calendar.
  if (!existing || existing->ownerId != actorId)
    co_return false;

  // A soft delete leaves the share rows alone, so the member list is still
  // there afterwards and every member receives the tombstone.
  const bool removed = co_await repository_.remove(id);
  if (removed)
    co_await emit(SyncOperation::Delete, *existing);
  co_return removed;
}
