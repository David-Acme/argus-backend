#pragma once

#include <argus/guard/v1/presence.grpc.pb.h>
#include <grpc/grpc-server-identity.hxx>

#include <vector>

class PresenceService;

struct PresenceRpcInput
{
  const PresenceService* presence{nullptr};
  std::vector<argus::client::CallerCredential> credentials;
};

class PresenceRpcService final
    : public argus::guard::v1::PresenceService::CallbackService
{
public:
  explicit PresenceRpcService(PresenceRpcInput input);

  grpc::ServerUnaryReactor*
  ListPresence(grpc::CallbackServerContext* context,
               const argus::guard::v1::ListPresenceRequest* request,
               argus::guard::v1::ListPresenceResponse* response) override;

private:
  PresenceRpcInput input_;
};
