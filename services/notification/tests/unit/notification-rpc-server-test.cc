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

TEST_CASE("a port that cannot be bound leaves no server, and the failure "
          "names both ports and what stops")
{
  AlwaysServing health;
  const NotificationRpcListenInput input{.address = "127.0.0.1:0",
                                         .tlsAddress = "192.0.2.1:0",
                                         .tls = pkiMaterial("notification"),
                                         .services = {&health}};
  const auto listener = startNotificationRpc(input);
  CHECK(listener.server == nullptr);
  CHECK(notificationRpcFailure(input, listener).value_or("") ==
        "gRPC server failed to listen on 127.0.0.1:0 and 192.0.2.1:0; without "
        "it guard cannot notify a user, productivity cannot announce the "
        "agenda, the llm cannot schedule a reminder call or post a notice, "
        "voice cannot report the end of a call, sync cannot pull "
        "notifications or claim a call, and settings cannot reach "
        "notification's settings or module requests");
}
