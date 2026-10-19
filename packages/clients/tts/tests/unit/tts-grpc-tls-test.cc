#include <doctest/doctest.h>

#include <grpc-test-support/mutual-tls-endpoint.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <tts.grpc.pb.h>
#include <tts/tts-client.hxx>

#include <chrono>
#include <string>

namespace
{

constexpr const char* kCallerSecret = "tts-caller-secret";

class LoadedSynthesis final : public argus::tts::v1::Synthesis::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  Capabilities(grpc::CallbackServerContext* context,
               const argus::tts::v1::CapabilitiesRequest*,
               argus::tts::v1::CapabilitiesResponse* out) override
  {
    out->set_sample_rate(24000);
    out->set_channels(1);
    out->set_format(argus::tts::v1::SAMPLE_FORMAT_FLOAT32);
    out->set_default_speed(1.0F);
    out->add_voices("jean");
    out->add_languages("es");
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
};

}

TEST_CASE("the tts client dials the mutual TLS port with the internal certificate")
{
  LoadedSynthesis service;
  const argus::client::test_support::MutualTlsEndpoint endpoint(
      {.service = "tts", .services = {&service}});
  REQUIRE(endpoint.tlsPort() > 0);
  argus::client::installInternalTls(
      argus::client::test_support::pkiMaterial("voice"));

  const argus::tts::Client client({.target = endpoint.tlsTarget(),
                                   .credential = kCallerSecret,
                                   .timeout = std::chrono::seconds(5)});
  CHECK(client.capabilities().sampleRate == 24000);

  const argus::tts::Client plaintext({.target = endpoint.plaintextToTlsPortTarget(),
                                      .credential = kCallerSecret,
                                      .timeout = std::chrono::seconds(5)});
  CHECK_THROWS(plaintext.capabilities());
}
