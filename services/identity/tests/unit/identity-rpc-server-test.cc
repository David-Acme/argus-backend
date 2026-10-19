#include <doctest/doctest.h>

#include <app/rpc/identity-rpc-server.hxx>
#include <app/rpc/identity-rpc-service.hxx>
#include <chrono>
#include <grpc-test-support/internal-tls-probe.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <identity-rpc-golden.hxx>
#include <json/json.h>
#include <optional>
#include <string>

namespace
{

using argus::client::test_support::AlwaysServing;
using argus::client::test_support::healthOver;
using argus::client::test_support::loopbackChannel;
using argus::client::test_support::pkiMaterial;

void stop(const argus::client::Listener& listener)
{
  if (listener.server)
    listener.server->Shutdown(std::chrono::system_clock::now() +
                              std::chrono::seconds(2));
}

}

TEST_CASE("the identity material and an address open two ports")
{
  AlwaysServing health;
  const IdentityRpcListenInput input{.address = "127.0.0.1:0",
                                     .tlsAddress = "127.0.0.1:0",
                                     .tls = pkiMaterial("identity"),
                                     .services = {&health}};
  const auto listener = startIdentityRpc(input);
  REQUIRE(listener.server != nullptr);
  CHECK(listener.port > 0);
  CHECK(listener.tlsPort > 0);
  CHECK(listener.tlsPort != listener.port);
  CHECK_FALSE(identityRpcFailure(input, listener).has_value());
  stop(listener);
}

TEST_CASE("without material only the plaintext port opens")
{
  AlwaysServing health;
  const auto listener = startIdentityRpc({.address = "127.0.0.1:0",
                                          .tlsAddress = "127.0.0.1:0",
                                          .tls = std::nullopt,
                                          .services = {&health}});
  REQUIRE(listener.server != nullptr);
  CHECK(listener.port > 0);
  CHECK(listener.tlsPort == 0);
  CHECK(healthOver(loopbackChannel(
            {.port = listener.port,
             .credentials = grpc::InsecureChannelCredentials(),
             .serverName = "argus-identity"})) == grpc::StatusCode::OK);
  stop(listener);
}

TEST_CASE("the mutual TLS port answers as argus-identity to a peer of the "
          "internal CA")
{
  AlwaysServing health;
  const auto listener = startIdentityRpc({.address = "127.0.0.1:0",
                                          .tlsAddress = "127.0.0.1:0",
                                          .tls = pkiMaterial("identity"),
                                          .services = {&health}});
  REQUIRE(listener.server != nullptr);
  REQUIRE(listener.tlsPort > 0);
  const auto peer =
      argus::client::internalChannelCredentials(pkiMaterial("auth"));
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-identity"})) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-auth"})) ==
        grpc::StatusCode::UNAVAILABLE);
  stop(listener);
}

TEST_CASE("a port that cannot be bound leaves no server, and identity must "
          "not start")
{
  AlwaysServing health;
  const IdentityRpcListenInput mutual{.address = "127.0.0.1:0",
                                      .tlsAddress = "192.0.2.1:0",
                                      .tls = pkiMaterial("identity"),
                                      .services = {&health}};
  const auto withTls = startIdentityRpc(mutual);
  CHECK(withTls.server == nullptr);
  CHECK(identityRpcFailure(mutual, withTls).value_or("") ==
        "Identity RPC failed to listen on 127.0.0.1:0 and 192.0.2.1:0; "
        "without it auth, camera, guard, notification, the llm, voice, sync "
        "and settings get no answer from identity");

  const IdentityRpcListenInput plain{.address = "192.0.2.1:0",
                                     .tlsAddress = "",
                                     .tls = std::nullopt,
                                     .services = {&health}};
  const auto withoutTls = startIdentityRpc(plain);
  CHECK(withoutTls.server == nullptr);
  CHECK(identityRpcFailure(plain, withoutTls).value_or("") ==
        "Identity RPC failed to listen on 192.0.2.1:0; without it auth, "
        "camera, guard, notification, the llm, voice, sync and settings get "
        "no answer from identity");
}

TEST_CASE("the identity answers through startIdentityRpc match the recorded "
          "golden, with and without TLS material")
{
  const IdentityGoldenApp app;
  IdentityRpcService service(
      {.bus = nullptr, .gate = goldenGate(), .auth = nullptr});
  const Json::Value golden = identityGoldenFile();

  for (const bool withMaterial : {false, true}) {
    const std::string mode = withMaterial ? "with TLS material" : "without TLS material";
    INFO(mode);
    const auto listener = startIdentityRpc(
        {.address = "127.0.0.1:0",
         .tlsAddress = withMaterial ? "127.0.0.1:0" : "",
         .tls = withMaterial
                    ? std::optional<argus::client::InternalTlsMaterial>(
                          pkiMaterial("identity"))
                    : std::nullopt,
         .services = {&service}});
    REQUIRE(listener.server != nullptr);
    CHECK((listener.tlsPort > 0) == withMaterial);

    const auto answers =
        askIdentityGolden("127.0.0.1:" + std::to_string(listener.port));
    CHECK(golden.size() == answers.size());
    for (const auto& answer : answers) {
      INFO(answer.name);
      REQUIRE(golden.isMember(answer.name));
      CHECK(golden[answer.name] == answer.answer);
    }
    stop(listener);
  }
}
