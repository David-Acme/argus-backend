#include "reminder-series.hxx"

#include <algorithm>
#include <shared/services/recurrence/occurrence-exceptions.hxx>
#include <shared/services/recurrence/recurrence.hxx>
#include <string>
#include <sync/user-change-sink.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

drogon::Task<std::optional<int64_t>> ReminderSeries::advance(const SeriesAdvanceInput& input) const
{
  const auto existing = co_await repository_.findOwned(
      {.id = input.reminderId, .targetUserId = input.targetUserId, .client = input.client});
  if (!existing || !existing->recurrenceRule)
    co_return std::nullopt;
  const auto rule = parseRecurrenceRule(*existing->recurrenceRule);
  if (!rule)
    co_return std::nullopt;
  const int64_t anchor = existing->recurrenceAnchor.value_or(existing->scheduledAt);
  const auto exceptions = parseOccurrenceExceptions(existing->recurrenceExceptions);
  const auto fire = nextFiring({.rule = *rule,
                                .anchor = anchor,
                                .after = scheduledOccurrence(exceptions, existing->scheduledAt),
                                .moment = std::max(existing->scheduledAt, input.moment),
                                .exceptions = exceptions});
  if (!fire)
    co_return std::nullopt;
  const auto kept = exceptionsFrom(exceptions, fire->occurrence);
  const std::optional<int64_t> next = fire->at;

  Json::Value before = existing->toJson();
  const auto row = co_await repository_.update({.id = input.reminderId,
                                                .targetUserId = input.targetUserId,
                                                .title = std::nullopt,
                                                .description = std::nullopt,
                                                .scheduledAt = next,
                                                .isCompleted = std::nullopt,
                                                .completedAt = std::nullopt,
                                                .clearCompletedAt = false,
                                                .recurrenceRule = std::nullopt,
                                                .recurrenceAnchor = std::nullopt,
                                                .clearRecurrence = false,
                                                .recurrenceExceptions =
                                                    kept.size() == exceptions.size()
                                                        ? std::nullopt
                                                        : std::optional<std::string>(
                                                              serializeOccurrenceExceptions(kept)),
                                                .client = input.client});
  if (row.id == 0)
    co_return std::nullopt;

  const auto* sink = user_change::getProductivitySink();
  if (!sink)
    LOG_WARN << "user change sink not installed; drop reminder series audit";
  else
    co_await sink->publishAudit({.recordId = row.id,
                                 .tableName = TableName::Reminder,
                                 .before = std::move(before),
                                 .after = row.toJson(),
                                 .userIds = {row.targetUserId},
                                 .client = input.client});
  co_return next;
}
