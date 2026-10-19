#include <doctest/doctest.h>

#include <app/rpc/auth-rpc-server.hxx>
#include <app/rpc/auth-rpc-service.hxx>
#include <chrono>
#include <feature/device/repositories/device-credential/device-credential-repository.hxx>
#include <feature/session/repositories/refresh-token/refresh-token-repository.hxx>
#include <feature/session/services/session-service.hxx>
#include <grpc-test-support/internal-tls-probe.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <optional>
#include <session-fixture.hxx>
#include <string>
#include <verdict-golden.hxx>

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

TEST_CASE("the auth material and an address open two ports")
{
  AlwaysServing health;
  const AuthRpcListenInput input{.address = "127.0.0.1:0",
                                 .tlsAddress = "127.0.0.1:0",
                                 .tls = pkiMaterial("auth"),
                                 .services = {&health}};
  const auto listener = startAuthRpc(input);
  REQUIRE(listener.server != nullptr);
  CHECK(listener.port > 0);
  CHECK(listener.tlsPort > 0);
  CHECK(listener.tlsPort != listener.port);
  CHECK_FALSE(authRpcFailure(input, listener).has_value());
  stop(listener);
}

TEST_CASE("without material only the plaintext port opens")
{
  AlwaysServing health;
  const auto listener = startAuthRpc({.address = "127.0.0.1:0",
                                      .tlsAddress = "127.0.0.1:0",
                                      .tls = std::nullopt,
                                      .services = {&health}});
  REQUIRE(listener.server != nullptr);
  CHECK(listener.port > 0);
  CHECK(listener.tlsPort == 0);
  CHECK(healthOver(loopbackChannel(
            {.port = listener.port,
             .credentials = grpc::InsecureChannelCredentials(),
             .serverName = "argus-auth"})) == grpc::StatusCode::OK);
  stop(listener);
}

TEST_CASE("the mutual TLS port answers as argus-auth to a peer of the internal "
          "CA")
{
  AlwaysServing health;
  const auto listener = startAuthRpc({.address = "127.0.0.1:0",
                                      .tlsAddress = "127.0.0.1:0",
                                      .tls = pkiMaterial("auth"),
                                      .services = {&health}});
  REQUIRE(listener.server != nullptr);
  REQUIRE(listener.tlsPort > 0);
  const auto peer =
      argus::client::internalChannelCredentials(pkiMaterial("camera"));
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-auth"})) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-identity"})) ==
        grpc::StatusCode::UNAVAILABLE);
  stop(listener);
}

TEST_CASE("a port that cannot be bound leaves no server, and auth must not "
          "start")
{
  AlwaysServing health;
  const AuthRpcListenInput mutual{.address = "127.0.0.1:0",
                                  .tlsAddress = "192.0.2.1:0",
                                  .tls = pkiMaterial("auth"),
                                  .services = {&health}};
  const auto withTls = startAuthRpc(mutual);
  CHECK(withTls.server == nullptr);
  CHECK(withTls.port == 0);
  CHECK(withTls.tlsPort == 0);
  CHECK(authRpcFailure(mutual, withTls).value_or("") ==
        "Auth RPC failed to listen on 127.0.0.1:0 and 192.0.2.1:0; without it "
        "no service can validate a session against this one");

  const AuthRpcListenInput plain{.address = "192.0.2.1:0",
                                 .tlsAddress = "",
                                 .tls = std::nullopt,
                                 .services = {&health}};
  const auto withoutTls = startAuthRpc(plain);
  CHECK(withoutTls.server == nullptr);
  CHECK(authRpcFailure(plain, withoutTls).value_or("") ==
        "Auth RPC failed to listen on 192.0.2.1:0; without it no service can "
        "validate a session against this one");
}

TEST_CASE("the plaintext verdict through startAuthRpc answers the recorded "
          "golden, with and without TLS material")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  AuthRpcService service({.sessions = &sessions,
                          .deviceCredentials = &credentials},
                         std::string{});
  const Json::Value golden = goldenFile();

  for (const bool withMaterial : {false, true}) {
    const std::string mode =
        withMaterial ? "with TLS material" : "without TLS material";
    INFO(mode);
    const auto listener = startAuthRpc(
        {.address = "127.0.0.1:0",
         .tlsAddress = withMaterial ? "127.0.0.1:0" : "",
         .tls = withMaterial
                    ? std::optional<argus::client::InternalTlsMaterial>(
                          pkiMaterial("auth"))
                    : std::nullopt,
         .services = {&service}});
    REQUIRE(listener.server != nullptr);
    CHECK((listener.tlsPort > 0) == withMaterial);

    const auto verdicts =
        askGoldenVerdicts("127.0.0.1:" + std::to_string(listener.port),
                          app.identity());
    CHECK(golden.size() == verdicts.size());
    for (const auto& verdict : verdicts) {
      INFO(verdict.name);
      REQUIRE(golden.isMember(verdict.name));
      CHECK(golden[verdict.name] == verdict.response);
    }
    stop(listener);
  }
}
