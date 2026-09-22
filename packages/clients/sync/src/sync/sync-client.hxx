#pragma once

#include <argus/sync/v1/sync.grpc.pb.h>
#include <cstdint>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-change.hxx>

// Where the control leg dials and the secret every call carries, named the way
// NotificationClientConfig names its target beside its credential: the two are
// a struct so a call site writes `.target = ...` and cannot hand the secret
// over as the address. An omitted fleetSecret means no secret, which
// addFleetSecret skips.
struct SyncClientConfig
{
  std::string target;
  std::string fleetSecret;
};

// Thin SDK wrapper over argus.sync.v1.SyncControlService (rule 23): the
// imperative leg of D8, for the operations that change no row and therefore
// cannot travel as a change event. The frame it carries is the change feed's
// {operation, table, info} triple in its typed spelling -- the table as the
// number, the row as JSON text -- and the service converts it back into the
// envelope's {operation, option, info} at the far end.
class SyncClient
{
public:
  explicit SyncClient(SyncClientConfig config);

  SyncClient(const SyncClient&) = delete;
  SyncClient& operator=(const SyncClient&) = delete;
  virtual ~SyncClient() = default;

  // One actor changing role. The role *names* cross, because the caller holds
  // the enum (C3): sync builds the room-control payload from them, so no
  // caller spells the routing keys by hand. Every method answers an ack, so
  // dropping one silently is a refusal nobody reads -- hence [[nodiscard]].
  [[nodiscard]] virtual bool
  replaceRoleRooms(const sync_change::RoleRoomChange& change) const;

  // Drop the user's sockets and refresh their auth context; the frame is the
  // AuthContextChanged row the socket renders, resync flag included.
  [[nodiscard]] virtual bool disconnectUser(int64_t userId,
                                            const SocketEmitDto& frame) const;

  // Directed emit to one user's room: the same frame, one recipient.
  [[nodiscard]] virtual bool emitToUser(int64_t userId,
                                        const SocketEmitDto& frame) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::sync::v1::SyncControlService::StubInterface> stub_;
  std::string fleetSecret_;
};
