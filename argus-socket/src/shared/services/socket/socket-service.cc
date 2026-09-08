#include "socket-service.hxx"

#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include "sync-change.hxx"

namespace
{
std::shared_ptr<NatsBus>& eventBus()
{
  static std::shared_ptr<NatsBus> bus;
  return bus;
}
} // namespace

void SocketService::setEventBus(std::shared_ptr<NatsBus> bus)
{
  eventBus() = std::move(bus);
}

void SocketService::publishChange(const Json::Value& payload)
{
  auto bus = eventBus();
  if (!bus)
    return;
  bus->publish(nats_subject::kSyncChange, json_util::toString(payload));
}

void SocketService::emitModule(TableName table, const SocketEmitDto& body) const
{
  const auto message = json_util::toString(body.toJson());
  roomManager_.emit(moduleRoom(table), message);
  publishChange(sync_change::emitPayload(body));
}

void SocketService::emitUser(int64_t userId, const SocketEmitDto& body) const
{
  const auto message = json_util::toString(body.toJson());
  roomManager_.emit(userRoom(userId), message);
  publishChange(sync_change::userEmitPayload(body, {userId}));
}

void SocketService::emitUsers(const std::vector<int64_t>& userIds,
                              const SocketEmitDto& body) const
{
  std::vector<RoomId> rooms;
  rooms.reserve(userIds.size());
  for (const auto userId : userIds)
    rooms.push_back(userRoom(userId));
  roomManager_.emitMany(rooms, json_util::toString(body.toJson()));
  publishChange(sync_change::userEmitPayload(body, userIds));
}

void SocketService::replaceRoleRooms(const RoleRoomReplaceInput& input) const
{
  roomManager_.replaceRoleRooms(input);
  publishChange(sync_change::roleRoomsPayload(input));
}

void SocketService::disconnectUser(int64_t userId,
                                   const SocketEmitDto& context) const
{
  roomManager_.disconnectUser(userId, json_util::toString(context.toJson()));
  publishChange(sync_change::disconnectPayload(context, userId));
}
