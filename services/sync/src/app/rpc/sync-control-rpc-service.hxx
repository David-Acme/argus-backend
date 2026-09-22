#pragma once

#include <argus/sync/v1/sync.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <string>

// The control plane's server half (D8): the three room operations that change no
// row, so they cannot travel as a change event. A caller's typed frame is
// rebuilt into the change feed's {operation, option, info} envelope and handed
// to the same dispatcher the NATS leg uses, so the two transports cannot
// diverge. Fleet-secret gated; the listener is cleartext.
class SyncControlRpcService final
    : public argus::sync::v1::SyncControlService::CallbackService
{
public:
  // fleetSecret is required in x-argus-fleet on every call; empty allows only loopback.
  explicit SyncControlRpcService(std::string fleetSecret);

  grpc::ServerUnaryReactor*
  ReplaceRoleRooms(grpc::CallbackServerContext* context,
                   const argus::sync::v1::ReplaceRoleRoomsRequest* request,
                   argus::sync::v1::ControlAck* response) override;

  grpc::ServerUnaryReactor*
  DisconnectUser(grpc::CallbackServerContext* context,
                 const argus::sync::v1::DisconnectUserRequest* request,
                 argus::sync::v1::ControlAck* response) override;

  grpc::ServerUnaryReactor*
  EmitToUser(grpc::CallbackServerContext* context,
             const argus::sync::v1::EmitToUserRequest* request,
             argus::sync::v1::ControlAck* response) override;

private:
  bool fleetAuthorized(const grpc::CallbackServerContext* context) const;

  std::string fleetSecret_;
};
