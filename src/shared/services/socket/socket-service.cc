#include "socket-service.hxx"

#include <shared/utils/json-util/json-util.hxx>

void SocketService::emitModule(TableName table, const SocketEmitDto& body) const
{
  roomManager_.emit(moduleRoom(table), json_util::toString(body.toJson()));
}

void SocketService::emitUser(int64_t userId, const SocketEmitDto& body) const
{
  roomManager_.emit(userRoom(userId), json_util::toString(body.toJson()));
}

void SocketService::emitUsers(const std::vector<int64_t>& userIds,
                              const SocketEmitDto& body) const
{
  std::vector<RoomId> rooms;
  rooms.reserve(userIds.size());
  for (const auto userId : userIds)
    rooms.push_back(userRoom(userId));
  roomManager_.emitMany(rooms, json_util::toString(body.toJson()));
}
