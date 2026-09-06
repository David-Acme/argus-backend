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

  // Persists the change: inserts a row or merges it into the record's
  // same-day row (replacement with a strictly increasing id). No room emit,
  // so a substrate that funnels from argus-productivity/argus-notification
  // (Rulings AQ/AR) controls the event itself.
  drogon::Task<UserAuditLogSchema>
  create(const UserAuditLogWriteInput& input) const;

  drogon::Task<UserAuditLogSchema>
  createAndEmit(const UserAuditLogWriteInput& input) const;

private:
  UserAuditLogRepository repository_;
  SocketService socketService_;
};
