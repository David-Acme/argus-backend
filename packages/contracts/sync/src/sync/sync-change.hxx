#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>
#include <sync/socket-emit-dto.hxx>
#include <vector>

// Wire contract of the argus.<domain>.v1.change feed (see
// docs/architecture/wire-nats-subjects.md): the SocketEmitDto triple plus the
// routing metadata the transport reads. Every key here is frozen.
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

// One actor changing role, as the wire carries it: the role *names*, because
// old_role/new_role travel as strings and the consumer parses them back. This
// is deliberately not packages/room's RoleRoomReplaceInput -- that one is the
// room manager's own call argument and carries the UserRole enum, and this row
// deletes packages/room, so a contract cannot be spelled in a type that dies
// with the row. The enum-to-name conversion belongs to the caller holding the
// enum (SocketService::replaceRoleRooms).
struct RoleRoomChange
{
  int64_t userId{0};
  std::string oldRole;
  std::string newRole;
};

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

inline Json::Value roleRoomsPayload(const RoleRoomChange& change)
{
  SocketEmitDto body;
  body.operation = SyncOperation::AuthContextChanged;
  body.option = TableName::User;
  body.obj["id"] = static_cast<Json::Int64>(change.userId);
  Json::Value payload = body.toJson();
  payload[kActionField] = kActionReplaceRoleRooms;
  payload[kUserField] = static_cast<Json::Int64>(change.userId);
  payload[kOldRoleField] = change.oldRole;
  payload[kNewRoleField] = change.newRole;
  return payload;
}
} // namespace sync_change
