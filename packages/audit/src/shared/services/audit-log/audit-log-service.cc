#include "audit-log-service.hxx"

#include <chrono>
#include <ctime>
#include <shared/utils/json-util/json-util.hxx>

namespace
{
std::pair<int64_t, int64_t> utcDayRangeMs(int64_t nowMs)
{
  const std::time_t t = static_cast<std::time_t>(nowMs / 1000);
  std::tm tm{};
  gmtime_r(&t, &tm);
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  const int64_t start = static_cast<int64_t>(timegm(&tm)) * 1000;
  return {start, start + 86'400'000};
}
} // namespace

drogon::Task<AuditLogSchema>
AuditLogService::create(const AuditLogWriteInput& input) const
{
  const int64_t now = input.eventTimestamp.value_or(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto [dayStart, dayEnd] = utcDayRangeMs(now);

  const auto existing = co_await repository_.findExist(
      {.recordId = input.recordId,
       .tableName = input.tableName,
       .dayStart = dayStart,
       .dayEnd = dayEnd});

  AuditLogSchema schema;
  if (!existing) {
    schema = co_await repository_.create(
        {.createUserId = input.createUserId,
         .recordId = input.recordId,
         .tableName = input.tableName,
         .changes = JsonDiff::toJson(input.changes),
         .priority = input.priority,
         .eventTimestamp = now});
    co_return schema;
  }

  const auto prev =
      JsonDiff::fromJsonString(json_util::toString(existing->changes));
  const auto merged = JsonDiff::compareChanges(prev, input.changes);
  const auto changes = merged.type == "DELETE" ? input.changes : merged.changes;
  co_await repository_.remove(existing->id);
  schema = co_await repository_.create(
      {.createUserId = input.createUserId ? input.createUserId
                                          : existing->createUserId,
       .recordId = input.recordId,
       .tableName = input.tableName,
       .changes = JsonDiff::toJson(changes),
       .priority = input.priority,
       .eventTimestamp = now});

  co_return schema;
}

drogon::Task<AuditLogSchema>
AuditLogService::createAndEmit(const AuditLogWriteInput& input) const
{
  const auto schema = co_await create(input);

  SocketEmitDto emit;
  emit.operation = SyncOperation::Log;
  emit.option = input.tableName;
  emit.obj = schema.toJson();
  socketService_.emitModule(input.tableName, emit);

  co_return schema;
}
