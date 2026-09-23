#pragma once

#include <argus/sync/v1/sync.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <string>

class SyncControlRpcService final
    : public argus::sync::v1::SyncControlService::CallbackService
{
public:
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
