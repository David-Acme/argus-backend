#pragma once

#include <cstdint>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>
#include <shared/services/room/room-manager.hxx>
#include <vector>

class SocketService
{
public:
  SocketService() = default;

  void emitModule(TableName table, const SocketEmitDto& body) const;
  void emitUser(int64_t userId, const SocketEmitDto& body) const;
  void emitUsers(const std::vector<int64_t>& userIds,
                 const SocketEmitDto& body) const;

private:
  RoomManager roomManager_;
};
