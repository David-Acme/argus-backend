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

  // Persists the change: inserts a row or merges it into the record's
  // same-day row (replacement with a strictly increasing id). No room emit,
  // so a substrate that fans out over its own channel — the camera domain
  // funnels from argus-camera (F2-2) — controls the event itself.
  drogon::Task<AuditLogSchema> create(const AuditLogWriteInput& input) const;

  drogon::Task<AuditLogSchema>
  createAndEmit(const AuditLogWriteInput& input) const;

private:
  AuditLogRepository repository_;
  SocketService socketService_;
};
