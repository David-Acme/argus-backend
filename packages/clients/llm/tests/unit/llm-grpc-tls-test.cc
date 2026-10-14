#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <grpc-test-support/mutual-tls-endpoint.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <llm.grpc.pb.h>
#include <llm/llm-client.hxx>

#include <chrono>
#include <string>

namespace
{

constexpr const char* kCallerSecret = "llm-caller-secret";

class LoadedChat final : public argus::llm::v1::Chat::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  Capabilities(grpc::CallbackServerContext* context,
               const argus::llm::v1::CapabilitiesRequest*,
               argus::llm::v1::CapabilitiesResponse* out) override
  {
    out->set_loaded(true);
    out->set_default_max_tokens(128);
    out->set_default_temperature(0.3F);
    out->set_context_size(4096);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
};

}

TEST_CASE("the llm client dials the mutual TLS port with the internal certificate")
{
  LoadedChat service;
  const argus::client::test_support::MutualTlsEndpoint endpoint(
      {.service = "llm", .services = {&service}});
  REQUIRE(endpoint.tlsPort() > 0);
  argus::client::installInternalTls(
      argus::client::test_support::pkiMaterial("voice"));

  const argus::llm::Client client({.target = endpoint.tlsTarget(),
                                   .credential = kCallerSecret,
                                   .timeout = std::chrono::seconds(5)});
  CHECK(client.capabilities().loaded);

  const argus::llm::Client plaintext({.target = endpoint.plaintextToTlsPortTarget(),
                                      .credential = kCallerSecret,
                                      .timeout = std::chrono::seconds(5)});
  CHECK_THROWS(plaintext.capabilities());
}
