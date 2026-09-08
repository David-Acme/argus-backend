#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/contracts/user-change-sink.hxx>
#include <shared/services/socket/socket-service.hxx>
#include <shared/services/sync-audit/sync-audit-service.hxx>

// Legacy substrate: local SocketService rooms plus the SyncAuditService publication.
class SocketUserChangeSink : public UserChangeSink
{
public:
  void emitUser(int64_t userId, const SocketEmitDto& body) const override;
  void emitUsers(const std::vector<int64_t>& userIds,
                 const SocketEmitDto& body) const override;
  drogon::Task<void>
  publishAudit(const UserAuditInput& input) const override;

private:
  SocketService socketService_;
  SyncAuditService syncAuditService_;
};
