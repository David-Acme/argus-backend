#pragma once

#include <argus/guard/v1/duress.grpc.pb.h>
#include <grpc/grpc-server-identity.hxx>

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class SafetyService;
class SafetyActorDirectory;

struct DuressRpcInput
{
  std::shared_ptr<const SafetyService> safety;
  const SafetyActorDirectory* actors{nullptr};
  std::vector<argus::client::CallerCredential> credentials;
  std::vector<argus::client::CallerCredential> panicCredentials{};
  std::function<drogon::Task<int64_t>(int64_t)> environmentOf{};
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

  grpc::ServerUnaryReactor* RaiseCameraPanic(grpc::CallbackServerContext* context,
                                             const argus::guard::v1::CameraPanicRequest* request,
                                             argus::guard::v1::CameraPanicResponse* response) override;

private:
  DuressRpcInput input_;
};
