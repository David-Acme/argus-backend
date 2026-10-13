#pragma once

#include <chrono>
#include <cstdlib>
#include <grpc/health/v1/health.grpc.pb.h>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace argus::client::test_support
{

class AlwaysServing final : public grpc::health::v1::Health::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  Check(grpc::CallbackServerContext* context,
        const grpc::health::v1::HealthCheckRequest*,
        grpc::health::v1::HealthCheckResponse* response) override
  {
    response->set_status(grpc::health::v1::HealthCheckResponse::SERVING);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
};

inline std::string requiredEnvironment(const char* name)
{
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    throw std::runtime_error(std::string(name) +
                             " is not set; run this test through "
                             "scripts/internal-tls-test.sh");
  return value;
}

inline InternalTlsMaterial materialIn(const std::string& directory)
{
  auto material = readInternalTls(internalTlsFilesIn(directory));
  if (!material)
    throw std::runtime_error("no internal TLS material in " + directory);
  return *std::move(material);
}

inline InternalTlsMaterial pkiMaterial(const std::string& service)
{
  return materialIn(requiredEnvironment("ARGUS_INTERNAL_TLS_PKI") + "/" +
                    service);
}

inline InternalTlsMaterial foreignMaterial()
{
  return materialIn(requiredEnvironment("ARGUS_INTERNAL_TLS_FOREIGN"));
}

inline std::shared_ptr<grpc::ChannelCredentials>
anonymousTls(const InternalTlsMaterial& trust)
{
  return grpc::SslCredentials({.pem_root_certs = trust.trustBundlePem,
                               .pem_private_key = "",
                               .pem_cert_chain = ""});
}

struct LoopbackChannelInput
{
  int port{0};
  std::shared_ptr<grpc::ChannelCredentials> credentials;
  std::string serverName;
};

inline std::shared_ptr<grpc::Channel>
loopbackChannel(const LoopbackChannelInput& input)
{
  grpc::ChannelArguments arguments;
  arguments.SetSslTargetNameOverride(input.serverName);
  return grpc::CreateCustomChannel("127.0.0.1:" + std::to_string(input.port),
                                   input.credentials, arguments);
}

inline grpc::StatusCode
healthOver(const std::shared_ptr<grpc::Channel>& channel)
{
  auto stub = grpc::health::v1::Health::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  const grpc::health::v1::HealthCheckRequest request;
  grpc::health::v1::HealthCheckResponse response;
  return stub->Check(&context, request, &response).error_code();
}

}
