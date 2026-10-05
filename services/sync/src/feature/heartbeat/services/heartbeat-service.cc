#include "heartbeat-service.hxx"

#include <sync/socket-emit-dto.hxx>
#include <sync/sync-operation.hxx>

#include <chrono>
#include <utility>

namespace
{
int64_t systemNow()
{
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

HeartbeatService::HeartbeatService(Dependencies dependencies,
                                   HeartbeatPolicy policy)
    : dependencies_(std::move(dependencies)), policy_(policy)
{
  if (!dependencies_.clock)
    dependencies_.clock = systemNow;
  if (!dependencies_.board)
    dependencies_.board = std::make_shared<const PresenceBoard>();
}

Json::Value HeartbeatService::heartbeatFor(int64_t userId) const
{
  const PresenceEntry presence = dependencies_.board->of(userId);
  return heartbeat::render({.now = dependencies_.clock(),
                            .presence = presence.overall,
                            .presenceSince = presence.since,
                            .guardSeenAt = dependencies_.board->guardSeenAt()},
                           policy_);
}

Json::Value HeartbeatService::frameFor(int64_t userId) const
{
  SocketEmitDto frame;
  frame.operation = SyncOperation::Heartbeat;
  frame.option = TableName::User;
  frame.obj = heartbeatFor(userId);
  return frame.toJson();
}
