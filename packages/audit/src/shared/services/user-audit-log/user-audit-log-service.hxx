#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/enums.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-query.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <shared/schemas/user-audit-log/user-audit-log-schema.hxx>
#include <shared/services/socket/socket-service.hxx>

class UserAuditLogService
{
public:
  UserAuditLogService() = default;

  // Persists the change: inserts a row or merges it into the record's same-day row (strictly increasing id).
  drogon::Task<UserAuditLogSchema>
  create(const UserAuditLogWriteInput& input) const;

  drogon::Task<UserAuditLogSchema>
  createAndEmit(const UserAuditLogWriteInput& input) const;

private:
  UserAuditLogRepository repository_;
  SocketService socketService_;
};
