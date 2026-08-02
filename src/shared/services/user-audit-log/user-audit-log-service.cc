#include "user-audit-log-service.hxx"

#include <ctime>
#include <shared/utils/json-util/json-util.hxx>

namespace
{
std::pair<int64_t, int64_t> utcDayRange(int64_t now)
{
  const std::time_t t = static_cast<std::time_t>(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  const int64_t start = static_cast<int64_t>(timegm(&tm));
  return {start, start + 86400};
}
} // namespace

drogon::Task<UserAuditLogSchema>
UserAuditLogService::createAndEmit(const UserAuditLogWriteInput& input) const
{
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const auto [dayStart, dayEnd] = utcDayRange(now);

  const auto existing = co_await repository_.findExist(
      {.userId = input.userId,
       .recordId = input.recordId,
       .tableName = input.tableName,
       .dayStart = dayStart,
       .dayEnd = dayEnd});

  UserAuditLogSchema schema;
  if (!existing) {
    schema = co_await repository_.create(
        {.userId = input.userId,
         .recordId = input.recordId,
         .tableName = input.tableName,
         .changes = JsonDiff::toJson(input.changes),
         .priority = input.priority,
         .eventTimestamp = now});
  }
  else {
    const auto prev =
        JsonDiff::fromJsonString(json_util::toString(existing->changes));
    const auto merged = JsonDiff::compareChanges(prev, input.changes);
    if (merged.type == "DELETE") {
      co_await repository_.remove(existing->id);
      schema = *existing;
    }
    else {
      co_await repository_.updateChanges(
          {.id = existing->id,
           .changes = JsonDiff::toJson(merged.changes),
           .eventTimestamp = now});
      schema = *existing;
      schema.changes = JsonDiff::toJson(merged.changes);
      schema.eventTimestamp = now;
    }
  }

  SocketEmitDto emit;
  emit.operation = SyncOperation::Log;
  emit.option = TableName::UserAuditLog;
  emit.obj = schema.toJson();
  socketService_.emitUser(input.userId, emit);

  co_return schema;
}
