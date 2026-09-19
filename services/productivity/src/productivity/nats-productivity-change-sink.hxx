#pragma once

#include <drogon/utils/coroutine.h>
#include <memory>
#include <shared/contracts/user-change-sink.hxx>

class NatsBus;

// Productivity-domain change funnel over argus.productivity.v1.change.
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
