#include "user-change-fan-out.hxx"

#include <drogon/drogon.h>
#include <shared/contracts/sync-operation.hxx>
#include <shared/contracts/user-audit-event.hxx>
#include <shared/enums.hxx>
#include <shared/services/user-audit-log/user-audit-log-service.hxx>
#include <sync/sync-fan-out.hxx>
#include <trantor/utils/Logger.h>

#include <unordered_set>

namespace user_change_fan_out
{
void handleUserChange(const Json::Value& json)
{
  if (json.isObject() && json.get("kind", "").asString() == "audit") {
    const auto event = UserAuditEvent::fromJson(json);
    if (!event) {
      LOG_WARN << "User change fan-out: dropped malformed audit event";
      return;
    }

    // Per recipient: insert first, fan the DB-assigned row out second.
    drogon::async_run([event = *event]() -> drogon::Task<void> {
      UserAuditLogService auditLogService;
      std::unordered_set<int64_t> recipients;
      for (const auto userId : event.users) {
        if (userId <= 0 || !recipients.insert(userId).second)
          continue;
        const auto schema = co_await auditLogService.create(
            {.userId = userId,
             .recordId = event.recordId,
             .tableName = event.tableName,
             .changes = event.changes,
             .priority = event.priority,
             .eventTimestamp = event.eventTimestamp});

        sync_fan_out::Event fanout;
        fanout.emit.operation = SyncOperation::Log;
        fanout.emit.option = TableName::UserAuditLog;
        fanout.emit.obj = schema.toJson();
        fanout.users = std::vector<int64_t>{userId};
        sync_fan_out::dispatchEvent(fanout);
      }
      co_return;
    });
    return;
  }

  const auto event = sync_fan_out::parseEvent(json);
  if (!event) {
    LOG_WARN << "User change fan-out: dropped malformed change event";
    return;
  }
  sync_fan_out::dispatchEvent(*event);
}
} // namespace user_change_fan_out
