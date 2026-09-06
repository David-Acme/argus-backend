#pragma once

#include <drogon/utils/coroutine.h>
#include <memory>
#include <shared/contracts/user-change-sink.hxx>

class NatsBus;

// Notification-domain substrate of argus-notification (Rulings AR/Y): the
// markAsRead audit rows and notification change emits funnel over
// `argus.notification.v1.change` instead of local rooms, and no audit rows
// are persisted locally. The gateway inserts the rows verbatim into its
// user_audit_log substrate before fanning them out.
class NatsNotificationChangeSink : public UserChangeSink
{
public:
  explicit NatsNotificationChangeSink(std::shared_ptr<NatsBus> bus);

  void emitUser(int64_t userId, const SocketEmitDto& body) const override;
  void emitUsers(const std::vector<int64_t>& userIds,
                 const SocketEmitDto& body) const override;
  drogon::Task<void>
  publishAudit(const UserAuditInput& input) const override;

private:
  std::shared_ptr<NatsBus> bus_;
};
