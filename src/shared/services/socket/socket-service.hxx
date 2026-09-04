#pragma once

#include <cstdint>
#include <memory>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>
#include <shared/services/room/room-manager.hxx>
#include <vector>

class NatsBus;

class SocketService
{
public:
  SocketService() = default;

  void emitModule(TableName table, const SocketEmitDto& body) const;
  void emitUser(int64_t userId, const SocketEmitDto& body) const;
  void emitUsers(const std::vector<int64_t>& userIds,
                 const SocketEmitDto& body) const;
  void replaceRoleRooms(const RoleRoomReplaceInput& input) const;
  void disconnectUser(int64_t userId, const SocketEmitDto& context) const;

  // Installs the process-wide event bus the emits publish to as a side
  // effect. Only the publisher host installs one (the gateway consumes);
  // unset means publishing is a no-op.
  static void setEventBus(std::shared_ptr<NatsBus> bus);

private:
  static void publishChange(const Json::Value& payload);

  RoomManager roomManager_;
};