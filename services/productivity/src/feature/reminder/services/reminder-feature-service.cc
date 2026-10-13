#include "reminder-feature-service.hxx"

#include <algorithm>
#include <ctime>
#include <errors/response-exception.hxx>
#include <productivity/productivity-errors.hxx>
#include <shared/services/recurrence/recurrence.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
constexpr const char* kIdempotencyRoute = "reminder";
}

drogon::Task<void> ReminderFeatureService::emit(const EmitInput& input) const
{
  const ReminderSchema& row = input.row;
  SocketEmitDto body;
  body.operation = input.operation;
  body.option = TableName::Reminder;
  if (input.operation == SyncOperation::Delete) {
    Json::Value tombstone;
    tombstone["id"] = row.id;
    tombstone["deletedAt"] = row.deletedAt.value_or(std::time(nullptr));
    body.obj = std::move(tombstone);
  }
  else {
    body.obj = row.toJson();
  }

  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop reminder emit";
    co_return;
  }
  co_await sink->emitUsers({.userIds = {row.targetUserId},
                            .body = std::move(body),
                            .client = input.client});
  co_return;
}

drogon::Task<ReminderSchema>
ReminderFeatureService::create(const ReminderCreateCommand& command) const
{
  const CreateReminderDto& body = command.body;
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ReminderSchema row;
  try {
    if (!command.idempotencyKey.empty()) {
      const auto earlier = co_await idempotency_.find({.userId = command.userId,
                                                       .key = command.idempotencyKey,
                                                       .client = transaction.get()});
      if (earlier) {
        if (earlier->route != kIdempotencyRoute)
          throw ResponseException(ProductivityErrors::IdempotencyKeyReused);
        const auto existing = co_await repository_.findOwned(
            {.id = earlier->recordId,
             .targetUserId = command.userId,
             .client = transaction.get()});
        if (!existing)
          throw ResponseException(ProductivityErrors::ReminderNotFound);
        db_transaction::rollback(transaction);
        co_return *existing;
      }
    }
    row = co_await repository_.create({.createdBy = command.userId,
                                       .targetUserId = command.userId,
                                       .title = body.title,
                                       .description = body.description,
                                       .scheduledAt = body.scheduledAt,
                                       .recurrenceRule = body.recurrenceRule,
                                       .recurrenceAnchor = body.recurrenceRule
                                           ? std::optional<int64_t>{body.scheduledAt}
                                           : std::nullopt,
                                       .client = transaction.get()});
    co_await emit({.operation = SyncOperation::Add,
                   .row = row,
                   .client = transaction.get()});
    co_await journal_.record({.actorId = command.userId,
                              .table = TableName::Reminder,
                              .recordId = row.id,
                              .action = UserAction::Create,
                              .title = "",
                              .sharingParentId = 0,
                              .client = transaction.get()});
    if (!command.idempotencyKey.empty())
      co_await idempotency_.remember({.userId = command.userId,
                                      .key = command.idempotencyKey,
                                      .route = kIdempotencyRoute,
                                      .recordId = row.id,
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

drogon::Task<std::optional<ReminderSchema>>
ReminderFeatureService::update(const ReminderUpdateCommand& command) const
{
  const UpdateReminderDto& body = command.body;
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ReminderSchema row;
  Json::Value before;
  try {
    const auto existing = co_await repository_.findOwned(
        {.id = command.id,
         .targetUserId = command.userId,
         .client = transaction.get()});
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }
    before = existing->toJson();

    const auto now = static_cast<int64_t>(std::time(nullptr));
    const bool stopping = body.recurrenceRule && body.recurrenceRule->empty();
    const bool ruleSet = body.recurrenceRule && !body.recurrenceRule->empty();
    const std::optional<std::string> effectiveRule =
        stopping ? std::nullopt : (ruleSet ? body.recurrenceRule : existing->recurrenceRule);
    const int64_t effectiveScheduled = body.scheduledAt.value_or(existing->scheduledAt);
    const bool scheduleChanged = body.scheduledAt && *body.scheduledAt != existing->scheduledAt;

    ReminderUpdateInput input{.id = command.id,
                              .targetUserId = command.userId,
                              .title = body.title,
                              .description = body.description,
                              .scheduledAt = body.scheduledAt,
                              .isCompleted = body.isCompleted,
                              .completedAt = std::nullopt,
                              .clearCompletedAt = false,
                              .recurrenceRule = std::nullopt,
                              .recurrenceAnchor = std::nullopt,
                              .clearRecurrence = stopping,
                              .client = transaction.get()};
    if (ruleSet) {
      input.recurrenceRule = body.recurrenceRule;
      input.recurrenceAnchor = effectiveScheduled;
    }
    else if (effectiveRule && scheduleChanged) {
      input.recurrenceAnchor = effectiveScheduled;
    }

    bool rolled = false;
    const auto rule = effectiveRule ? parseRecurrenceRule(*effectiveRule) : std::nullopt;
    if (body.isCompleted.value_or(false) && rule) {
      const int64_t anchor = input.recurrenceAnchor.value_or(
          existing->recurrenceAnchor.value_or(effectiveScheduled));
      const auto next = firstOccurrenceAfter(
          {.rule = *rule, .anchor = anchor, .moment = std::max(effectiveScheduled, now)});
      if (next) {
        input.scheduledAt = *next;
        input.isCompleted = false;
        input.clearCompletedAt = existing->isCompleted;
        rolled = true;
      }
    }
    if (body.isCompleted && !rolled) {
      if (*body.isCompleted && !existing->isCompleted)
        input.completedAt = now;
      if (!*body.isCompleted)
        input.clearCompletedAt = true;
    }

    row = co_await repository_.update(input);
    if (row.id == 0) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }

    const Json::Value after = row.toJson();
    if (after != before) {
      const auto* sink = user_change::getProductivitySink();
      if (!sink)
        LOG_WARN << "user change sink not installed; drop reminder audit";
      else
        co_await sink->publishAudit({.recordId = row.id,
                                     .tableName = TableName::Reminder,
                                     .before = std::move(before),
                                     .after = after,
                                     .userIds = {row.targetUserId},
                                     .client = transaction.get()});
      co_await journal_.record({.actorId = command.userId,
                                .table = TableName::Reminder,
                                .recordId = row.id,
                                .action = UserAction::Update,
                                .title = "",
                                .sharingParentId = 0,
                                .client = transaction.get()});
    }

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return row;
}

drogon::Task<std::optional<ReminderSchema>>
ReminderFeatureService::snooze(const ReminderSnoozeCommand& command) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ReminderSchema row;
  Json::Value before;
  try {
    const auto existing = co_await repository_.findOwned(
        {.id = command.id,
         .targetUserId = command.userId,
         .client = transaction.get()});
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }
    if (existing->isCompleted) {
      db_transaction::rollback(transaction);
      throw ResponseException(ProductivityErrors::ReminderAlreadyDone);
    }
    before = existing->toJson();

    const auto now = static_cast<int64_t>(std::time(nullptr));
    const auto target = command.body.tomorrow
                            ? sameTimeTomorrow(std::max(existing->scheduledAt, now))
                            : std::optional<int64_t>{now + *command.body.minutes * 60};
    if (!target) {
      db_transaction::rollback(transaction);
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
    }

    row = co_await repository_.update({.id = command.id,
                                       .targetUserId = command.userId,
                                       .title = std::nullopt,
                                       .description = std::nullopt,
                                       .scheduledAt = *target,
                                       .isCompleted = std::nullopt,
                                       .completedAt = std::nullopt,
                                       .clearCompletedAt = false,
                                       .recurrenceRule = std::nullopt,
                                       .recurrenceAnchor = std::nullopt,
                                       .clearRecurrence = false,
                                       .client = transaction.get()});
    if (row.id == 0) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }

    const Json::Value after = row.toJson();
    if (after != before) {
      const auto* sink = user_change::getProductivitySink();
      if (!sink)
        LOG_WARN << "user change sink not installed; drop reminder audit";
      else
        co_await sink->publishAudit({.recordId = row.id,
                                     .tableName = TableName::Reminder,
                                     .before = std::move(before),
                                     .after = after,
                                     .userIds = {row.targetUserId},
                                     .client = transaction.get()});
      co_await journal_.record({.actorId = command.userId,
                                .table = TableName::Reminder,
                                .recordId = row.id,
                                .action = UserAction::Update,
                                .title = "",
                                .sharingParentId = 0,
                                .client = transaction.get()});
    }

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return row;
}

drogon::Task<bool> ReminderFeatureService::remove(const ReminderRef& ref) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  try {
    auto existing = co_await repository_.findOwned(
        {.id = ref.id, .targetUserId = ref.userId, .client = transaction.get()});
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    if (!co_await repository_.remove({.id = ref.id,
                                      .targetUserId = ref.userId,
                                      .client = transaction.get()})) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    existing->deletedAt = static_cast<int64_t>(std::time(nullptr));
    co_await emit({.operation = SyncOperation::Delete,
                   .row = *existing,
                   .client = transaction.get()});
    co_await journal_.record({.actorId = ref.userId,
                              .table = TableName::Reminder,
                              .recordId = (*existing).id,
                              .action = UserAction::Delete,
                              .title = "",
                              .sharingParentId = 0,
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

drogon::Task<std::optional<ReminderSchema>>
ReminderFeatureService::get(const ReminderRef& ref) const
{
  co_return co_await repository_.findOwned(
      {.id = ref.id, .targetUserId = ref.userId, .client = nullptr});
}

drogon::Task<std::vector<ReminderSchema>>
ReminderFeatureService::list(const ReminderListCommand& command) const
{
  const int64_t limit =
      std::clamp<int64_t>(command.limit, 1, reminder_query::kListLimit);
  co_return co_await repository_.findByTargetUser(
      {.targetUserId = command.userId,
       .includeCompleted = command.includeCompleted,
       .limit = limit});
}
