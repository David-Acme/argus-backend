#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/notification-rpc-server.hxx>
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

TEST_CASE("the notification material and an address open two ports")
{
  AlwaysServing health;
  const auto listener =
      startNotificationRpc({.address = "127.0.0.1:0",
                            .tlsAddress = "127.0.0.1:0",
                            .tls = pkiMaterial("notification"),
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
  const auto listener = startNotificationRpc({.address = "127.0.0.1:0",
                                              .tlsAddress = "127.0.0.1:0",
                                              .tls = std::nullopt,
                                              .services = {&health}});
  REQUIRE(listener.server != nullptr);
  CHECK(listener.port > 0);
  CHECK(listener.tlsPort == 0);
  CHECK(healthOver(loopbackChannel(
            {.port = listener.port,
             .credentials = grpc::InsecureChannelCredentials(),
             .serverName = "argus-notification"})) == grpc::StatusCode::OK);
  stop(listener);
}

TEST_CASE("the mutual TLS port answers as argus-notification to a peer of the "
          "internal CA")
{
  AlwaysServing health;
  const auto listener =
      startNotificationRpc({.address = "127.0.0.1:0",
                            .tlsAddress = "127.0.0.1:0",
                            .tls = pkiMaterial("notification"),
                            .services = {&health}});
  REQUIRE(listener.server != nullptr);
  REQUIRE(listener.tlsPort > 0);
  const auto peer =
      argus::client::internalChannelCredentials(pkiMaterial("camera"));
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-notification"})) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-auth"})) ==
        grpc::StatusCode::UNAVAILABLE);
  stop(listener);
}
