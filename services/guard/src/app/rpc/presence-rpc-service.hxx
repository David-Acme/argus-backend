#pragma once

#include <argus/guard/v1/presence.grpc.pb.h>
#include <grpc/grpc-server-identity.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <vector>

class PresenceService;

using DutyLookup = std::function<drogon::Task<std::vector<int64_t>>(int64_t)>;

struct PresenceRpcInput
{
  const PresenceService* presence{nullptr};
  std::vector<argus::client::CallerCredential> credentials;
  DutyLookup duty{};
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

  grpc::ServerUnaryReactor*
  GetDuty(grpc::CallbackServerContext* context,
          const argus::guard::v1::DutyRequest* request,
          argus::guard::v1::DutyResponse* response) override;

private:
  PresenceRpcInput input_;
};
