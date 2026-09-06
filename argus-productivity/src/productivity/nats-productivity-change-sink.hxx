#pragma once

#include <drogon/utils/coroutine.h>
#include <memory>
#include <shared/contracts/user-change-sink.hxx>

class NatsBus;

// Productivity-domain substrate of argus-productivity (Rulings AQ/Y): change
// emits and user audit diffs funnel over `argus.productivity.v1.change`
// instead of local rooms, and no audit rows are persisted locally. The
// gateway inserts the rows verbatim into its user_audit_log substrate before
// fanning them out.
class NatsProductivityChangeSink : public UserChangeSink
{
public:
  explicit NatsProductivityChangeSink(std::shared_ptr<NatsBus> bus);

  void emitUser(int64_t userId, const SocketEmitDto& body) const override;
  void emitUsers(const std::vector<int64_t>& userIds,
                 const SocketEmitDto& body) const override;
  drogon::Task<void>
  publishAudit(const UserAuditInput& input) const override;

private:
  std::shared_ptr<NatsBus> bus_;
};