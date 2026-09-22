#pragma once

#include <cstdint>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-change.hxx>

// The imperative leg of the change feed: the operations that change no row and
// therefore cannot travel as an event. contracts/sync holds the interface so a
// domain package can call it without linking the transport; the host installs
// clients/sync's SyncClient at boot.
class SyncControlSink
{
public:
  virtual ~SyncControlSink() = default;

  [[nodiscard]] virtual bool
  replaceRoleRooms(const sync_change::RoleRoomChange& change) const = 0;

  [[nodiscard]] virtual bool disconnectUser(int64_t userId,
                                            const SocketEmitDto& frame) const = 0;

  [[nodiscard]] virtual bool emitToUser(int64_t userId,
                                        const SocketEmitDto& frame) const = 0;
};

namespace sync_control
{
inline const SyncControlSink*& sink()
{
  static const SyncControlSink* instance = nullptr;
  return instance;
}

inline void setSink(const SyncControlSink* value)
{
  sink() = value;
}

inline const SyncControlSink* getSink()
{
  return sink();
}
} // namespace sync_control
