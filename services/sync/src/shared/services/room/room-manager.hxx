#pragma once

#include <chrono>
#include <functional>

#include <cstdint>
#include <drogon/WebSocketConnection.h>
#include <drogon/drogon.h>
#include <memory>
#include <sync/table-name.hxx>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <auth/role-access.hxx>
#include <auth/user-role.hxx>
#include <vector>

using Conn = drogon::WebSocketConnectionPtr;
using RoomId = uint64_t;

struct RoleRoomReplaceInput
{
  int64_t userId;
  UserRole oldRole;
  UserRole newRole;
};

struct SessionDisconnectInput
{
  int64_t userId{0};
  std::string sessionId;
  std::string contextMessage;
  std::string cause;
};

struct SocketFarewellInput
{
  const drogon::WebSocketConnectionPtr& conn;
  std::string_view cause;
};

using SocketFarewell = std::function<std::chrono::milliseconds(const SocketFarewellInput&)>;

inline RoomId moduleRoom(TableName table)
{
  return 1 + static_cast<uint64_t>(table);
}

inline RoomId reducedModuleRoom(TableName table)
{
  return 500 + static_cast<uint64_t>(table);
}

inline RoomId moduleRoomFor(TableName table, UserRole role)
{
  if (table == TableName::Camera && !role_access::readsCameraConnection(role))
    return reducedModuleRoom(table);
  return moduleRoom(table);
}

inline std::vector<RoomId> moduleRoomsOf(UserRole role)
{
  std::vector<RoomId> rooms;
  for (const auto table : role_access::moduleTables(role))
    rooms.push_back(moduleRoomFor(table, role));
  return rooms;
}

inline RoomId userRoom(int64_t userId)
{
  return 1000 + static_cast<uint64_t>(userId);
}

class RoomManager
{
public:
  RoomManager() = default;

  void init();
  void shutdown();

  void join(RoomId room, const Conn& conn) const;
  void joinMany(const std::vector<RoomId>& rooms, const Conn& conn) const;
  void leave(RoomId room, const Conn& conn) const;
  void leaveAll(const Conn& conn) const;

  void emit(RoomId room, std::string_view msg) const;
  void emitMany(const std::vector<RoomId>& rooms, std::string_view msg) const;
  void replaceRoleRooms(const RoleRoomReplaceInput& input) const;
  void disconnectUser(int64_t userId, std::string_view contextMessage) const;
  void disconnectSession(const SessionDisconnectInput& input) const;

  [[nodiscard]] bool isOnline(RoomId room) const;

  static void setSocketFarewell(SocketFarewell farewell);

private:
  static void emitLocalRoomsView(const std::vector<RoomId>& rooms,
                                 std::string_view msg);
  static void broadcastToLocalThreads(const std::vector<RoomId>& rooms,
                                      const std::shared_ptr<std::string>& msg);
  static void replaceLocalRoleRooms(const RoleRoomReplaceInput& input);
  static void disconnectLocalUserRoom(
      RoomId room, const std::shared_ptr<std::string>& contextMessage);
  static void
  disconnectLocalSession(const std::shared_ptr<const SessionDisconnectInput>& input);
  struct SocketClose
  {
    Conn conn;
    std::shared_ptr<const std::string> message;
    std::string closeReason;
    std::string cause;
  };

  static void closeAfterFarewell(SocketClose close);
  static void leaveAllLocal(drogon::WebSocketConnection* raw);
  static void pruneDeadConnection(drogon::WebSocketConnection* raw);
  static void pruneAllDeadConnections();
};
