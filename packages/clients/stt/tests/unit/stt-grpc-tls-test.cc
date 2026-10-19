#include <doctest/doctest.h>

#include <grpc-test-support/mutual-tls-endpoint.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <stt.grpc.pb.h>
#include <stt/stt-client.hxx>

#include <chrono>
#include <string>

namespace
{

constexpr const char* kCallerSecret = "stt-caller-secret";

class LoadedTranscription final : public argus::stt::v1::Transcription::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  Capabilities(grpc::CallbackServerContext* context,
               const argus::stt::v1::CapabilitiesRequest*,
               argus::stt::v1::CapabilitiesResponse* out) override
  {
    out->set_sample_rate(16000);
    out->set_loaded(true);
    out->set_language("es");
    out->set_default_language("es");
    out->add_languages("es");
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
};

}

TEST_CASE("the stt client dials the mutual TLS port with the internal certificate")
{
  LoadedTranscription service;
  const argus::client::test_support::MutualTlsEndpoint endpoint(
      {.service = "stt", .services = {&service}});
  REQUIRE(endpoint.tlsPort() > 0);
  argus::client::installInternalTls(
      argus::client::test_support::pkiMaterial("voice"));

  const argus::stt::Client client({.target = endpoint.tlsTarget(),
                                   .credential = kCallerSecret,
                                   .timeout = std::chrono::seconds(5)});
  CHECK(client.capabilities().sampleRate == 16000);

  const argus::stt::Client plaintext({.target = endpoint.plaintextToTlsPortTarget(),
                                      .credential = kCallerSecret,
                                      .timeout = std::chrono::seconds(5)});
  CHECK_THROWS(static_cast<void>(plaintext.capabilities()));
}
