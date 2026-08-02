#pragma once

#include <cstdint>
#include <drogon/WebSocketConnection.h>
#include <drogon/drogon.h>
#include <memory>
#include <shared/enums.hxx>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Conn = drogon::WebSocketConnectionPtr;
using RoomId = uint64_t;

inline RoomId moduleRoom(TableName table)
{
  return 1 + static_cast<uint64_t>(table);
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

  bool isOnline(RoomId room) const;

private:
  static void emitLocalRoomsView(const std::vector<RoomId>& rooms,
                                 std::string_view msg);
  static void broadcastToLocalThreads(const std::vector<RoomId>& rooms,
                                      const std::shared_ptr<std::string>& msg);
  static void pruneDeadConnection(drogon::WebSocketConnection* raw);
  static void pruneAllDeadConnections();
};
