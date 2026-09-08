#pragma once

#include <argus/identity/v1/identity.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>

// IdentityService controller: enforces x-argus-user against request->user_id().
class IdentityRpcService final
    : public argus::identity::v1::IdentityService::CallbackService
{
public:
  explicit IdentityRpcService(std::shared_ptr<NatsBus> bus);

  grpc::ServerUnaryReactor*
  UpdateUser(grpc::CallbackServerContext* context,
             const argus::identity::v1::UpdateUserRequest* request,
             argus::identity::v1::UpdateUserResponse* response) override;

private:
  UserRepository userRepository_;
  std::shared_ptr<NatsBus> bus_;
};
