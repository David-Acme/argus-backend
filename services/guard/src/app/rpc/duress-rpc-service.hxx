#pragma once

#include <argus/guard/v1/duress.grpc.pb.h>
#include <grpc/grpc-server-identity.hxx>

#include <memory>
#include <vector>

class SafetyService;
class SafetyActorDirectory;

struct DuressRpcInput
{
  std::shared_ptr<const SafetyService> safety;
  const SafetyActorDirectory* actors{nullptr};
  std::vector<argus::client::CallerCredential> credentials;
};

class DuressRpcService final : public argus::guard::v1::DuressService::CallbackService
{
public:
  explicit DuressRpcService(DuressRpcInput input);

  grpc::ServerUnaryReactor* GetPhrase(grpc::CallbackServerContext* context,
                                      const argus::guard::v1::DuressPhraseRequest* request,
                                      argus::guard::v1::DuressPhraseResponse* response) override;

  grpc::ServerUnaryReactor* RaiseSpoken(grpc::CallbackServerContext* context,
                                        const argus::guard::v1::SpokenDuressRequest* request,
                                        argus::guard::v1::SpokenDuressResponse* response) override;

private:
  DuressRpcInput input_;
};
