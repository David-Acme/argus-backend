#pragma once

#include <argus/identity/v1/sync.grpc.pb.h>
#include <grpcpp/grpcpp.h>
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
    std::string fleetSecret;
  };

  explicit IdentitySyncRpcService(Dependencies dependencies);

  grpc::ServerUnaryReactor*
  PullTable(grpc::CallbackServerContext* context,
            const argus::identity::v1::PullTableRequest* request,
            argus::identity::v1::PullTableResponse* response) override;

private:
  bool fleetAuthorized(const grpc::CallbackServerContext* context) const;

  std::string fleetSecret_;
  UserRepository userRepository_;
  UserInvitationRepository userInvitationRepository_;
  PersonRepository personRepository_;
};
