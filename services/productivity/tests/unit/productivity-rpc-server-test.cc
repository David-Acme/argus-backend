#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/productivity-rpc-server.hxx>
#include <chrono>
#include <cstdlib>
#include <grpc/health/v1/health.grpc.pb.h>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{

using argus::client::InternalTlsMaterial;

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

std::string environment(const char* name)
{
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    throw std::runtime_error(
        std::string(name) +
        " is not set; run this test through scripts/internal-tls-test.sh");
  return value;
}

InternalTlsMaterial materialOf(const std::string& directory)
{
  auto material = argus::client::readInternalTls(
      argus::client::internalTlsFilesIn(directory));
  if (!material)
    throw std::runtime_error("no internal TLS material in " + directory);
  return *std::move(material);
}

InternalTlsMaterial serviceMaterial(const std::string& service)
{
  return materialOf(environment("ARGUS_INTERNAL_TLS_PKI") + "/" + service);
}

grpc::StatusCode healthOver(const std::shared_ptr<grpc::Channel>& channel)
{
  auto stub = grpc::health::v1::Health::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  const grpc::health::v1::HealthCheckRequest request;
  grpc::health::v1::HealthCheckResponse response;
  return stub->Check(&context, request, &response).error_code();
}

std::shared_ptr<grpc::Channel>
channelTo(int port,
          const std::shared_ptr<grpc::ChannelCredentials>& credentials)
{
  grpc::ChannelArguments arguments;
  arguments.SetSslTargetNameOverride("argus-productivity");
  return grpc::CreateCustomChannel("127.0.0.1:" + std::to_string(port),
                                   credentials, arguments);
}

ProductivityRpcListenInput listenInput(std::optional<InternalTlsMaterial> tls,
                                       std::string tlsAddress)
{
  return {.address = "127.0.0.1:0",
          .tlsAddress = std::move(tlsAddress),
          .tls = std::move(tls),
          .services = {}};
}

}

TEST_CASE("material and an address give a plaintext port and a mutual TLS port")
{
  const auto ports = productivityRpcPorts(
      listenInput(serviceMaterial("productivity"), "127.0.0.1:7237"));
  REQUIRE(ports.size() == 2);
  CHECK(ports[0].address == "127.0.0.1:0");
  CHECK_FALSE(ports[0].mutualTls);
  CHECK(ports[0].credentials != nullptr);
  CHECK(ports[1].address == "127.0.0.1:7237");
  CHECK(ports[1].mutualTls);
  CHECK(ports[1].credentials != nullptr);
  CHECK(ports[1].credentials != ports[0].credentials);
}

TEST_CASE("without material there is a plaintext port only")
{
  const auto ports = productivityRpcPorts(listenInput(std::nullopt, ""));
  REQUIRE(ports.size() == 1);
  CHECK_FALSE(ports[0].mutualTls);
}

TEST_CASE(
    "an address without material is a plaintext port only, as the rule says")
{
  const auto ports =
      productivityRpcPorts(listenInput(std::nullopt, "127.0.0.1:7237"));
  REQUIRE(ports.size() == 1);
  CHECK(ports[0].address == "127.0.0.1:0");
  CHECK_FALSE(ports[0].mutualTls);
}

TEST_CASE(
    "material without an address loads it for clients and opens no second port")
{
  const auto ports =
      productivityRpcPorts(listenInput(serviceMaterial("productivity"), ""));
  REQUIRE(ports.size() == 1);
  CHECK_FALSE(ports[0].mutualTls);
}

TEST_CASE("no plaintext address opens nothing, mutual TLS included")
{
  auto input = listenInput(serviceMaterial("productivity"), "127.0.0.1:0");
  input.address.clear();
  CHECK(productivityRpcPorts(input).empty());
  const auto listener = startProductivityRpc(input);
  CHECK(listener.server == nullptr);
  CHECK(listener.port == 0);
  CHECK(listener.tlsPort == 0);
}

TEST_CASE("the mutual TLS port serves a peer of the internal CA and refuses "
          "every other")
{
  AlwaysServing health;
  auto input = listenInput(serviceMaterial("productivity"), "127.0.0.1:0");
  input.services = {&health};
  auto listener = startProductivityRpc(input);
  REQUIRE(listener.server != nullptr);
  REQUIRE(listener.port > 0);
  REQUIRE(listener.tlsPort > 0);
  CHECK(listener.tlsPort != listener.port);

  CHECK(healthOver(
            channelTo(listener.port, grpc::InsecureChannelCredentials())) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(channelTo(listener.tlsPort,
                             argus::client::internalChannelCredentials(
                                 serviceMaterial("camera")))) ==
        grpc::StatusCode::OK);
  CHECK(
      healthOver(channelTo(listener.tlsPort,
                           argus::client::internalChannelCredentials(materialOf(
                               environment("ARGUS_INTERNAL_TLS_FOREIGN"))))) ==
      grpc::StatusCode::UNAVAILABLE);
  const auto camera = serviceMaterial("camera");
  CHECK(healthOver(channelTo(listener.tlsPort,
                             grpc::SslCredentials(
                                 {.pem_root_certs = camera.trustBundlePem,
                                  .pem_private_key = "",
                                  .pem_cert_chain = ""}))) ==
        grpc::StatusCode::UNAVAILABLE);
  CHECK(healthOver(
            channelTo(listener.tlsPort, grpc::InsecureChannelCredentials())) ==
        grpc::StatusCode::UNAVAILABLE);
  listener.server->Shutdown(std::chrono::system_clock::now() +
                            std::chrono::seconds(2));
}

TEST_CASE("a mutual TLS address that cannot be bound starts nothing")
{
  AlwaysServing health;
  auto input = listenInput(serviceMaterial("productivity"), "192.0.2.1:0");
  input.services = {&health};
  const auto listener = startProductivityRpc(input);
  CHECK(listener.server == nullptr);
  CHECK(listener.port == 0);
  CHECK(listener.tlsPort == 0);
}

TEST_CASE("a listener without a service, or with a null one, is refused")
{
  CHECK_THROWS_AS(static_cast<void>(
                      startProductivityRpc(listenInput(std::nullopt, ""))),
                  std::invalid_argument);
  auto input = listenInput(std::nullopt, "");
  input.services = {nullptr};
  CHECK_THROWS_AS(static_cast<void>(startProductivityRpc(input)),
                  std::invalid_argument);
}
