#include "sync-audit-service.hxx"

#include <shared/services/audit-log/audit-log-service.hxx>
#include <shared/services/user-audit-log/user-audit-log-service.hxx>
#include <shared/utils/json-diff/json-diff.hxx>
#include <unordered_set>

drogon::Task<void>
SyncAuditService::publishModule(const SyncAuditModuleInput& input) const
{
  const auto changes = JsonDiff::createFlatDiff(input.before, input.after);
  if (changes.empty())
    co_return;

  AuditLogService auditLogService;
  co_await auditLogService.createAndEmit({
      .recordId = input.recordId,
      .tableName = input.tableName,
      .changes = changes,
      .createUserId = input.actorId,
      .eventTimestamp = std::nullopt,
  });
  co_return;
}

drogon::Task<void>
SyncAuditService::publishUsers(const SyncAuditUsersInput& input) const
{
  const auto changes = JsonDiff::createFlatDiff(input.before, input.after);
  if (changes.empty())
    co_return;

  UserAuditLogService auditLogService;
  std::unordered_set<int64_t> recipients;
  for (const auto userId : input.userIds) {
    if (userId <= 0 || !recipients.insert(userId).second)
      continue;
    co_await auditLogService.createAndEmit({
        .userId = userId,
        .recordId = input.recordId,
        .tableName = input.tableName,
        .changes = changes,
        .eventTimestamp = std::nullopt,
    });
  }
  co_return;
}
