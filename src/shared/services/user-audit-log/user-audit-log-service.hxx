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

  drogon::Task<UserAuditLogSchema>
  createAndEmit(const UserAuditLogWriteInput& input) const;

private:
  UserAuditLogRepository repository_;
  SocketService socketService_;
};
