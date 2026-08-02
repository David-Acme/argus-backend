#include "room-manager.hxx"

#include <atomic>
#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

namespace
{
struct RoomData
{
  std::vector<drogon::WebSocketConnection*> members;
  std::unordered_map<drogon::WebSocketConnection*, size_t> index;

  void add(drogon::WebSocketConnection* p)
  {
    if (index.count(p))
      return;
    index[p] = members.size();
    members.push_back(p);
  }

  void remove(drogon::WebSocketConnection* p)
  {
    const auto it = index.find(p);
    if (it == index.end())
      return;
    const size_t idx = it->second;
    const size_t last = members.size() - 1;
    if (idx != last) {
      auto* moved = members[last];
      members[idx] = moved;
      index[moved] = idx;
    }
    members.pop_back();
    index.erase(it);
  }
};

struct Local
{
  std::unordered_map<RoomId, RoomData> rooms;
  std::unordered_map<drogon::WebSocketConnection*, std::unordered_set<RoomId>>
      reverse;
  std::unordered_map<drogon::WebSocketConnection*,
                     std::weak_ptr<drogon::WebSocketConnection>>
      weakRefs;
};

// El estado del registry es thread_local (compartido por todas las instancias
// de RoomManager en un mismo hilo IO). Vive a nivel de archivo para que la
// clase siga siendo una instancia normal (testeable, inyectable).
thread_local Local g_local;
std::atomic<bool> g_initialized{false};
std::vector<std::pair<trantor::EventLoop*, trantor::TimerId>> g_pruneTimers;
} // namespace

void RoomManager::init()
{
  bool expected = false;
  if (!g_initialized.compare_exchange_strong(expected, true))
    return;

  drogon::app().registerBeginningAdvice([]() {
    const size_t threadCount = drogon::app().getThreadNum();
    for (size_t i = 0; i < threadCount; ++i) {
      auto* loop = drogon::app().getIOLoop(i);
      const auto timerId = loop->runEvery(60.0, []() {
        RoomManager::pruneAllDeadConnections();
      });
      g_pruneTimers.emplace_back(loop, timerId);
    }
  });
}

void RoomManager::shutdown()
{
  if (!g_initialized.exchange(false))
    return;

  for (const auto& [loop, timerId] : g_pruneTimers) {
    if (loop && timerId)
      loop->invalidateTimer(timerId);
  }
  g_pruneTimers.clear();
}

void RoomManager::join(RoomId room, const Conn& conn) const
{
  if (!conn)
    return;
  auto* raw = conn.get();
  g_local.weakRefs[raw] = conn;
  g_local.rooms[room].add(raw);
  g_local.reverse[raw].insert(room);
}

void RoomManager::joinMany(const std::vector<RoomId>& rooms,
                           const Conn& conn) const
{
  if (!conn)
    return;
  auto* raw = conn.get();
  g_local.weakRefs[raw] = conn;
  for (const auto room : rooms) {
    g_local.rooms[room].add(raw);
    g_local.reverse[raw].insert(room);
  }
}

void RoomManager::leave(RoomId room, const Conn& conn) const
{
  if (!conn)
    return;
  auto* raw = conn.get();

  const auto it = g_local.rooms.find(room);
  if (it != g_local.rooms.end()) {
    it->second.remove(raw);
    if (it->second.members.empty())
      g_local.rooms.erase(it);
  }

  const auto rev = g_local.reverse.find(raw);
  if (rev != g_local.reverse.end()) {
    rev->second.erase(room);
    if (rev->second.empty()) {
      g_local.reverse.erase(rev);
      g_local.weakRefs.erase(raw);
    }
  }
}

void RoomManager::leaveAll(const Conn& conn) const
{
  if (!conn)
    return;
  auto* raw = conn.get();

  const auto it = g_local.reverse.find(raw);
  if (it == g_local.reverse.end())
    return;

  for (const auto room : it->second) {
    const auto rit = g_local.rooms.find(room);
    if (rit != g_local.rooms.end()) {
      rit->second.remove(raw);
      if (rit->second.members.empty())
        g_local.rooms.erase(rit);
    }
  }

  g_local.reverse.erase(it);
  g_local.weakRefs.erase(raw);
}

void RoomManager::emitLocalRoomsView(const std::vector<RoomId>& rooms,
                                     std::string_view msg)
{
  std::unordered_set<drogon::WebSocketConnection*> seen;
  seen.reserve(64);

  for (const auto room : rooms) {
    const auto it = g_local.rooms.find(room);
    if (it == g_local.rooms.end())
      continue;

    for (auto* raw : it->second.members) {
      if (!seen.insert(raw).second)
        continue;

      const auto wIt = g_local.weakRefs.find(raw);
      if (wIt == g_local.weakRefs.end())
        continue;

      const auto sp = wIt->second.lock();
      if (!sp) {
        pruneDeadConnection(raw);
        continue;
      }
      sp->send(msg.data(), msg.size());
    }
  }
}

void RoomManager::broadcastToLocalThreads(
    const std::vector<RoomId>& rooms,
    const std::shared_ptr<std::string>& msg)
{
  const size_t threadCount = drogon::app().getThreadNum();
  auto* currentLoop = trantor::EventLoop::getEventLoopOfCurrentThread();

  for (size_t i = 0; i < threadCount; ++i) {
    auto* loop = drogon::app().getIOLoop(i);
    if (loop == currentLoop)
      continue;
    loop->queueInLoop([rooms, msg]() {
      RoomManager::emitLocalRoomsView(rooms, std::string_view(*msg));
    });
  }
}

void RoomManager::emit(RoomId room, std::string_view msg) const
{
  const std::vector<RoomId> rooms{room};
  const auto shared = std::make_shared<std::string>(msg);
  emitLocalRoomsView(rooms, *shared);
  broadcastToLocalThreads(rooms, shared);
}

void RoomManager::emitMany(const std::vector<RoomId>& rooms,
                           std::string_view msg) const
{
  const auto shared = std::make_shared<std::string>(msg);
  emitLocalRoomsView(rooms, *shared);
  broadcastToLocalThreads(rooms, shared);
}

bool RoomManager::isOnline(RoomId room) const
{
  const auto it = g_local.rooms.find(room);
  if (it == g_local.rooms.end())
    return false;

  for (auto* raw : it->second.members) {
    const auto wIt = g_local.weakRefs.find(raw);
    if (wIt != g_local.weakRefs.end()) {
      const auto conn = wIt->second.lock();
      if (conn && conn->connected())
        return true;
    }
  }
  return false;
}

void RoomManager::pruneDeadConnection(drogon::WebSocketConnection* raw)
{
  const auto revIt = g_local.reverse.find(raw);
  if (revIt != g_local.reverse.end()) {
    for (const auto rid : revIt->second) {
      const auto rit = g_local.rooms.find(rid);
      if (rit != g_local.rooms.end())
        rit->second.remove(raw);
    }
    g_local.reverse.erase(revIt);
  }
  g_local.weakRefs.erase(raw);
}

void RoomManager::pruneAllDeadConnections()
{
  std::vector<drogon::WebSocketConnection*> dead;
  dead.reserve(g_local.weakRefs.size());

  for (const auto& [raw, weak] : g_local.weakRefs) {
    if (weak.expired())
      dead.push_back(raw);
  }

  for (auto* raw : dead)
    pruneDeadConnection(raw);
}
