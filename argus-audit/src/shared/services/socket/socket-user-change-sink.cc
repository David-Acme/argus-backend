#include "socket-user-change-sink.hxx"

void SocketUserChangeSink::emitUser(int64_t userId,
                                    const SocketEmitDto& body) const
{
  socketService_.emitUser(userId, body);
}

void SocketUserChangeSink::emitUsers(const std::vector<int64_t>& userIds,
                                     const SocketEmitDto& body) const
{
  socketService_.emitUsers(userIds, body);
}

drogon::Task<void> SocketUserChangeSink::publishAudit(
    const UserAuditInput& input) const
{
  co_await syncAuditService_.publishUsers({
      .recordId = input.recordId,
      .tableName = input.tableName,
      .before = input.before,
      .after = input.after,
      .userIds = input.userIds,
  });
  co_return;
}
