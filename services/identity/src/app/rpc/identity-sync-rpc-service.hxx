#pragma once

#include <argus/identity/v1/sync.grpc.pb.h>
#include <grpc/fleet-caller-gate.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <string>

class IdentitySyncRpcService final
    : public argus::identity::v1::SyncService::CallbackService
{
public:
  struct Dependencies
  {
    std::shared_ptr<const argus::client::FleetCallerGate> gate;
  };

  explicit IdentitySyncRpcService(Dependencies dependencies);

  grpc::ServerUnaryReactor*
  PullTable(grpc::CallbackServerContext* context,
            const argus::identity::v1::PullTableRequest* request,
            argus::identity::v1::PullTableResponse* response) override;

private:
  std::shared_ptr<const argus::client::FleetCallerGate> gate_;
  UserRepository userRepository_;
  UserInvitationRepository userInvitationRepository_;
  PersonRepository personRepository_;
};
