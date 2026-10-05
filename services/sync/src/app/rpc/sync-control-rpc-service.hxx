#pragma once

#include <argus/sync/v1/sync.grpc.pb.h>
#include <grpc/fleet-caller-gate.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>

class SyncControlRpcService final
    : public argus::sync::v1::SyncControlService::CallbackService
{
public:
  explicit SyncControlRpcService(
      std::shared_ptr<const argus::client::FleetCallerGate> gate);

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
};
