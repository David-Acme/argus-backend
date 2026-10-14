#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/modules-rpc-server.hxx>
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

TEST_CASE("the settings material and an address open two ports")
{
  AlwaysServing health;
  const auto listener = startModulesRpc({.address = "127.0.0.1:0",
                                         .tlsAddress = "127.0.0.1:0",
                                         .tls = pkiMaterial("settings"),
                                         .services = {&health}});
  REQUIRE(listener.server != nullptr);
  CHECK(listener.port > 0);
  CHECK(listener.tlsPort > 0);
  CHECK(listener.tlsPort != listener.port);
  stop(listener);
}

TEST_CASE("without material only the plaintext port opens")
{
  AlwaysServing health;
  const auto listener = startModulesRpc({.address = "127.0.0.1:0",
                                         .tlsAddress = "127.0.0.1:0",
                                         .tls = std::nullopt,
                                         .services = {&health}});
  REQUIRE(listener.server != nullptr);
  CHECK(listener.port > 0);
  CHECK(listener.tlsPort == 0);
  CHECK(healthOver(loopbackChannel(
            {.port = listener.port,
             .credentials = grpc::InsecureChannelCredentials(),
             .serverName = "argus-settings"})) == grpc::StatusCode::OK);
  stop(listener);
}

TEST_CASE("the mutual TLS port answers as argus-settings to a peer of the "
          "internal CA")
{
  AlwaysServing health;
  const auto listener = startModulesRpc({.address = "127.0.0.1:0",
                                         .tlsAddress = "127.0.0.1:0",
                                         .tls = pkiMaterial("settings"),
                                         .services = {&health}});
  REQUIRE(listener.server != nullptr);
  REQUIRE(listener.tlsPort > 0);
  const auto peer =
      argus::client::internalChannelCredentials(pkiMaterial("camera"));
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-settings"})) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-auth"})) ==
        grpc::StatusCode::UNAVAILABLE);
  stop(listener);
}

TEST_CASE("a port that cannot be bound leaves no server, and the failure "
          "names both ports and what stops")
{
  AlwaysServing health;
  const ModulesRpcListenInput input{.address = "127.0.0.1:0",
                                    .tlsAddress = "192.0.2.1:0",
                                    .tls = pkiMaterial("settings"),
                                    .services = {&health}};
  const auto listener = startModulesRpc(input);
  CHECK(listener.server == nullptr);
  CHECK(modulesRpcFailure(input, listener).value_or("") ==
        "Modules RPC failed to listen on 127.0.0.1:0 and 192.0.2.1:0; without "
        "it camera, guard, identity, llm, notification, productivity and sync "
        "read no enabled set at start and keep their last known module state "
        "until the module feed brings it, sync cannot read the Owner's "
        "catalog, and the llm cannot use the modules tools");
}
