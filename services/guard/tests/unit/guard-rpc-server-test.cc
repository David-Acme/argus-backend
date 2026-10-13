#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/guard-rpc-server.hxx>
#include <chrono>
#include <grpc-test-support/internal-tls-probe.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
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

TEST_CASE("the guard material and an address open two ports")
{
  AlwaysServing health;
  const GuardRpcListenInput input{.address = "127.0.0.1:0",
                                  .tlsAddress = "127.0.0.1:0",
                                  .tls = pkiMaterial("guard"),
                                  .services = {&health}};
  const auto listener = startGuardRpc(input);
  REQUIRE(listener.server != nullptr);
  CHECK(listener.port > 0);
  CHECK(listener.tlsPort > 0);
  CHECK(listener.tlsPort != listener.port);
  CHECK_FALSE(guardRpcFailure(input, listener).has_value());
  stop(listener);
}

TEST_CASE("without material only the plaintext port opens")
{
  AlwaysServing health;
  const auto listener = startGuardRpc({.address = "127.0.0.1:0",
                                       .tlsAddress = "127.0.0.1:0",
                                       .tls = std::nullopt,
                                       .services = {&health}});
  REQUIRE(listener.server != nullptr);
  CHECK(listener.port > 0);
  CHECK(listener.tlsPort == 0);
  CHECK(healthOver(loopbackChannel(
            {.port = listener.port,
             .credentials = grpc::InsecureChannelCredentials(),
             .serverName = "argus-guard"})) == grpc::StatusCode::OK);
  stop(listener);
}

TEST_CASE("the mutual TLS port answers as argus-guard to a peer of the "
          "internal CA")
{
  AlwaysServing health;
  const auto listener = startGuardRpc({.address = "127.0.0.1:0",
                                       .tlsAddress = "127.0.0.1:0",
                                       .tls = pkiMaterial("guard"),
                                       .services = {&health}});
  REQUIRE(listener.server != nullptr);
  REQUIRE(listener.tlsPort > 0);
  const auto peer =
      argus::client::internalChannelCredentials(pkiMaterial("llm"));
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-guard"})) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-sync"})) ==
        grpc::StatusCode::UNAVAILABLE);
  stop(listener);
}

TEST_CASE("a port that cannot be bound leaves no server, and guard must not "
          "start")
{
  AlwaysServing health;
  const GuardRpcListenInput mutual{.address = "127.0.0.1:0",
                                   .tlsAddress = "192.0.2.1:0",
                                   .tls = pkiMaterial("guard"),
                                   .services = {&health}};
  const auto withTls = startGuardRpc(mutual);
  CHECK(withTls.server == nullptr);
  CHECK(withTls.port == 0);
  CHECK(withTls.tlsPort == 0);
  CHECK(guardRpcFailure(mutual, withTls).value_or("") ==
        "Guard RPC listener failed to listen on 127.0.0.1:0 and 192.0.2.1:0");

  const GuardRpcListenInput plain{.address = "192.0.2.1:0",
                                  .tlsAddress = "",
                                  .tls = std::nullopt,
                                  .services = {&health}};
  const auto withoutTls = startGuardRpc(plain);
  CHECK(withoutTls.server == nullptr);
  CHECK(guardRpcFailure(plain, withoutTls).value_or("") ==
        "Guard RPC listener failed to listen on 192.0.2.1:0");
}
