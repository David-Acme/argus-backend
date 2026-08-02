#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/enums.hxx>
#include <shared/repositories/audit-log/audit-log-query.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/schemas/audit-log/audit-log-schema.hxx>
#include <shared/services/socket/socket-service.hxx>

class AuditLogService
{
public:
  AuditLogService() = default;

  drogon::Task<AuditLogSchema>
  createAndEmit(const AuditLogWriteInput& input) const;

private:
  AuditLogRepository repository_;
  SocketService socketService_;
};
