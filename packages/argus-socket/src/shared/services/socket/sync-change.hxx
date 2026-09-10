#pragma once

#include <json/value.h>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>
#include <shared/services/room/room-manager.hxx>
#include <vector>

// Wire contract of the argus.sync.v1.change fan-out (see docs/architecture/wire-nats-subjects.md): the SocketEmitDto triple plus gateway routing metadata.
namespace sync_change
{
inline constexpr const char* kUsersField = "users";
inline constexpr const char* kActionField = "action";
inline constexpr const char* kActionEmit = "emit";
inline constexpr const char* kActionDisconnect = "disconnect";
inline constexpr const char* kActionReplaceRoleRooms = "replace_role_rooms";
inline constexpr const char* kUserField = "user";
inline constexpr const char* kOldRoleField = "old_role";
inline constexpr const char* kNewRoleField = "new_role";

inline Json::Value emitPayload(const SocketEmitDto& body,
                               const std::vector<int64_t>& users = {})
{
  Json::Value payload = body.toJson();
  if (!users.empty()) {
    Json::Value rooms(Json::arrayValue);
    for (const auto userId : users)
      rooms.append(static_cast<Json::Int64>(userId));
    payload[kUsersField] = rooms;
  }
  return payload;
}

inline Json::Value userEmitPayload(const SocketEmitDto& body,
                                   const std::vector<int64_t>& users)
{
  Json::Value payload = body.toJson();
  Json::Value rooms(Json::arrayValue);
  for (const auto userId : users)
    rooms.append(static_cast<Json::Int64>(userId));
  payload[kUsersField] = rooms;
  return payload;
}

inline Json::Value disconnectPayload(const SocketEmitDto& context,
                                     int64_t userId)
{
  Json::Value payload = userEmitPayload(context, {userId});
  payload[kActionField] = kActionDisconnect;
  payload[kUserField] = static_cast<Json::Int64>(userId);
  return payload;
}

inline Json::Value roleRoomsPayload(const RoleRoomReplaceInput& input)
{
  SocketEmitDto body;
  body.operation = SyncOperation::AuthContextChanged;
  body.option = TableName::User;
  body.obj["id"] = static_cast<Json::Int64>(input.userId);
  Json::Value payload = body.toJson();
  payload[kActionField] = kActionReplaceRoleRooms;
  payload[kUserField] = static_cast<Json::Int64>(input.userId);
  payload[kOldRoleField] = userRoleToString(input.oldRole);
  payload[kNewRoleField] = userRoleToString(input.newRole);
  return payload;
}
} // namespace sync_change
