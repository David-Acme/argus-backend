#include "audit-fan-out.hxx"

#include <feature/fanout/services/sync-fan-out.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <stdexcept>
#include <string>
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
}

bool AuditFanOut::migrateLegacySchema() const
{
  return userActionLogRepository_.migrateLegacySchema();
}

drogon::Task<void> AuditFanOut::insertModuleAudit(const ModuleAuditEvent& event)
{
  auto transaction = co_await db_transaction::begin(DbService::client());
  AuditLogSchema schema;
  try {
    schema = co_await auditLogService_.create({
        .recordId = event.recordId,
        .tableName = event.tableName,
        .changes = event.changes,
        .priority = event.priority,
        .createUserId = event.createUserId,
        .eventTimestamp = event.eventTimestamp,
        .client = transaction.get(),
    });
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw std::runtime_error("module audit row was not committed");
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  sync_fan_out::Event fanout;
  fanout.emit.operation = SyncOperation::Log;
  fanout.emit.option = schema.tableName;
  fanout.emit.obj = schema.toJson();
  sync_fan_out::dispatchEvent(fanout);
}

drogon::Task<void> AuditFanOut::insertUsersAudit(const UserAuditEvent& event)
{
  std::unordered_set<int64_t> recipients;
  std::vector<UserAuditLogSchema> written;
  written.reserve(event.users.size());
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    for (const auto userId : event.users) {
      if (userId <= 0 || !recipients.insert(userId).second)
        continue;
      written.push_back(co_await userAuditLogService_.create(
          {.userId = userId,
           .recordId = event.recordId,
           .tableName = event.tableName,
           .changes = event.changes,
           .priority = event.priority,
           .eventTimestamp = event.eventTimestamp,
           .client = transaction.get()}));
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw std::runtime_error("user audit rows were not committed");
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  for (const auto& schema : written) {
    const int64_t userId = schema.userId;
    sync_fan_out::Event fanout;
    fanout.emit.operation = SyncOperation::Log;
    fanout.emit.option = TableName::UserAuditLog;
    fanout.emit.obj = schema.toJson();
    fanout.users = std::vector<int64_t>{userId};
    sync_fan_out::dispatchEvent(fanout);
  }
}

drogon::Task<void> AuditFanOut::insertAction(const UserActionEvent& event,
                                             std::string_view msgId)
{
  const auto inserted =
      co_await userActionLogRepository_.create({.userId = event.userId,
                                                .recordId = event.recordId,
                                                .tableName = event.tableName,
                                                .action = event.action,
                                                .oldData = event.oldData,
                                                .newData = event.newData,
                                                .ipAddress = event.ipAddress,
                                                .msgId = std::string(msgId)});
  if (!inserted)
    LOG_INFO << "Action journal: " << msgId
             << " is already recorded; the redelivery is ignored";
}

drogon::Task<bool> AuditFanOut::handleAuditChange(const Json::Value& json)
{
  if (namesRecipients(json)) {
    const auto event = UserAuditEvent::fromJson(json);
    if (!event) {
      LOG_WARN << "Audit fan-out: malformed user audit event refused";
      co_return false;
    }
    co_await insertUsersAudit(*event);
    co_return true;
  }

  const auto event = ModuleAuditEvent::fromJson(json);
  if (!event) {
    LOG_WARN << "Audit fan-out: malformed module audit event refused";
    co_return false;
  }
  co_await insertModuleAudit(*event);
  co_return true;
}

drogon::Task<bool> AuditFanOut::handleActionJournal(const Json::Value& json,
                                                    std::string_view msgId)
{
  const auto event = UserActionEvent::fromJson(json);
  if (!event) {
    LOG_WARN << "Action journal: malformed event refused";
    co_return false;
  }
  co_await insertAction(*event, msgId);
  co_return true;
}
