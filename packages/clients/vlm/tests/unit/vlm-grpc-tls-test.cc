#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <grpc-test-support/mutual-tls-endpoint.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <vlm.grpc.pb.h>
#include <vlm/vlm-client.hxx>

#include <chrono>
#include <string>

namespace
{

constexpr const char* kCallerSecret = "vlm-caller-secret";

class LoadedVision final : public argus::vlm::v1::Vision::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  Capabilities(grpc::CallbackServerContext* context,
               const argus::vlm::v1::CapabilitiesRequest*,
               argus::vlm::v1::CapabilitiesResponse* out) override
  {
    out->set_loaded(true);
    out->set_max_input_px(384);
    out->set_default_max_tokens(64);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
};

}

TEST_CASE("the vlm client dials the mutual TLS port with the internal certificate")
{
  LoadedVision service;
  const argus::client::test_support::MutualTlsEndpoint endpoint(
      {.service = "vlm", .services = {&service}});
  REQUIRE(endpoint.tlsPort() > 0);
  argus::client::installInternalTls(
      argus::client::test_support::pkiMaterial("guard"));

  const argus::vlm::Client client({.target = endpoint.tlsTarget(),
                                   .credential = kCallerSecret,
                                   .timeout = std::chrono::seconds(5)});
  CHECK(client.capabilities().loaded);

  const argus::vlm::Client plaintext({.target = endpoint.plaintextToTlsPortTarget(),
                                      .credential = kCallerSecret,
                                      .timeout = std::chrono::seconds(5)});
  CHECK_THROWS(plaintext.capabilities());
}
