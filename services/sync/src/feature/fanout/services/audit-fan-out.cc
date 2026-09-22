#include "audit-fan-out.hxx"

#include <drogon/drogon.h>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <trantor/utils/Logger.h>
#include <unordered_set>
#include <utility>

namespace
{
bool namesRecipients(const Json::Value& json)
{
  return json.isMember("users") && json["users"].isArray() &&
         !json["users"].empty();
}
} // namespace

void AuditFanOut::insertModuleAudit(const ModuleAuditEvent& event)
{
  drogon::async_run([this, event]() -> drogon::Task<void> {
    try {
      const auto schema = co_await auditLogService_.create({
          .recordId = event.recordId,
          .tableName = event.tableName,
          .changes = event.changes,
          .priority = event.priority,
          .createUserId = event.createUserId,
          .eventTimestamp = event.eventTimestamp,
      });

      sync_fan_out::Event fanout;
      fanout.emit.operation = SyncOperation::Log;
      fanout.emit.option = schema.tableName;
      fanout.emit.obj = schema.toJson();
      sync_fan_out::dispatchEvent(fanout);
    }
    catch (const std::exception& error) {
      LOG_WARN << "Audit fan-out: module insert failed, event dropped: "
               << error.what();
    }
    catch (...) {
      LOG_WARN << "Audit fan-out: module insert failed with unknown error; "
                  "event dropped";
    }
    co_return;
  });
}

void AuditFanOut::insertUsersAudit(const UserAuditEvent& event)
{
  drogon::async_run([this, event]() -> drogon::Task<void> {
    try {
      std::unordered_set<int64_t> recipients;
      for (const auto userId : event.users) {
        if (userId <= 0 || !recipients.insert(userId).second)
          continue;
        const auto schema = co_await userAuditLogService_.create(
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
    }
    catch (const std::exception& error) {
      LOG_WARN << "Audit fan-out: user insert failed, event dropped: "
               << error.what();
    }
    catch (...) {
      LOG_WARN << "Audit fan-out: user insert failed with unknown error; "
                  "event dropped";
    }
    co_return;
  });
}

void AuditFanOut::insertAction(const UserActionEvent& event)
{
  drogon::async_run([this, event]() -> drogon::Task<void> {
    try {
      co_await userActionLogRepository_.create({.userId = event.userId,
                                                .recordId = event.recordId,
                                                .tableName = event.tableName,
                                                .action = event.action,
                                                .oldData = event.oldData,
                                                .newData = event.newData,
                                                .ipAddress = event.ipAddress});
    }
    catch (const std::exception& error) {
      LOG_WARN << "Action journal: insert failed, row dropped: "
               << error.what();
    }
    catch (...) {
      LOG_WARN << "Action journal: insert failed with unknown error; row "
                  "dropped";
    }
    co_return;
  });
}

void AuditFanOut::handleAuditChange(const Json::Value& json)
{
  if (namesRecipients(json)) {
    const auto event = UserAuditEvent::fromJson(json);
    if (!event) {
      LOG_WARN << "Audit fan-out: dropped malformed user audit event";
      return;
    }
    insertUsersAudit(*event);
    return;
  }

  const auto event = ModuleAuditEvent::fromJson(json);
  if (!event) {
    LOG_WARN << "Audit fan-out: dropped malformed module audit event";
    return;
  }
  insertModuleAudit(*event);
}

void AuditFanOut::handleActionJournal(const Json::Value& json)
{
  const auto event = UserActionEvent::fromJson(json);
  if (!event) {
    LOG_WARN << "Action journal: dropped malformed event";
    return;
  }
  insertAction(*event);
}
