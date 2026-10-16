#pragma once

#include <argus/sync/v1/sync.grpc.pb.h>
#include <feature/rtc/infra/livekit-room-client.hxx>
#include <grpc/fleet-caller-gate.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>

class SyncControlRpcService final
    : public argus::sync::v1::SyncControlService::CallbackService
{
public:
  SyncControlRpcService(
      std::shared_ptr<const argus::client::FleetCallerGate> gate,
      std::shared_ptr<const LiveKitRoomClient> rooms);

  grpc::ServerUnaryReactor*
  CloseCallRoom(grpc::CallbackServerContext* context,
                const argus::sync::v1::CloseCallRoomRequest* request,
                argus::sync::v1::CloseCallRoomAck* response) override;

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
  [[nodiscard]] argus::client::FleetAdmission
  admit(const grpc::CallbackServerContext* context,
        argus::client::CallerSet allowed) const;

  std::shared_ptr<const argus::client::FleetCallerGate> gate_;
  std::shared_ptr<const LiveKitRoomClient> rooms_;
};
