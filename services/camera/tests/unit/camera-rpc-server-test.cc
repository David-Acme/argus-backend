#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/camera-rpc-server.hxx>
#include <chrono>
#include <grpc-test-support/internal-tls-probe.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <optional>
#include <stdexcept>
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

TEST_CASE("the camera material and an address open two ports")
{
  AlwaysServing health;
  const CameraRpcListenInput input{.address = "127.0.0.1:0",
                                   .tlsAddress = "127.0.0.1:0",
                                   .tls = pkiMaterial("camera"),
                                   .services = {&health}};
  const auto listener = startCameraRpc(input);
  REQUIRE(listener.server != nullptr);
  CHECK(listener.port > 0);
  CHECK(listener.tlsPort > 0);
  CHECK(listener.tlsPort != listener.port);
  CHECK_FALSE(cameraRpcFailure(input, listener).has_value());
  stop(listener);
}

TEST_CASE("without material only the plaintext port opens")
{
  AlwaysServing health;
  const auto listener = startCameraRpc({.address = "127.0.0.1:0",
                                        .tlsAddress = "127.0.0.1:0",
                                        .tls = std::nullopt,
                                        .services = {&health}});
  REQUIRE(listener.server != nullptr);
  CHECK(listener.port > 0);
  CHECK(listener.tlsPort == 0);
  CHECK(healthOver(loopbackChannel(
            {.port = listener.port,
             .credentials = grpc::InsecureChannelCredentials(),
             .serverName = "argus-camera"})) == grpc::StatusCode::OK);
  stop(listener);
}

TEST_CASE("the mutual TLS port answers as argus-camera to a peer of the "
          "internal CA")
{
  AlwaysServing health;
  const auto listener = startCameraRpc({.address = "127.0.0.1:0",
                                        .tlsAddress = "127.0.0.1:0",
                                        .tls = pkiMaterial("camera"),
                                        .services = {&health}});
  REQUIRE(listener.server != nullptr);
  REQUIRE(listener.tlsPort > 0);
  const auto peer =
      argus::client::internalChannelCredentials(pkiMaterial("guard"));
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-camera"})) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(loopbackChannel({.port = listener.tlsPort,
                                    .credentials = peer,
                                    .serverName = "argus-auth"})) ==
        grpc::StatusCode::UNAVAILABLE);
  stop(listener);
}

TEST_CASE("a listener without a service, or with a null one, is refused")
{
  CHECK_THROWS_AS(static_cast<void>(startCameraRpc({.address = "127.0.0.1:0",
                                                    .tlsAddress = "",
                                                    .tls = std::nullopt,
                                                    .services = {}})),
                  std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(startCameraRpc({.address = "127.0.0.1:0",
                                                    .tlsAddress = "",
                                                    .tls = std::nullopt,
                                                    .services = {nullptr}})),
                  std::invalid_argument);
}

TEST_CASE("a port that cannot be bound leaves no server, and the failure "
          "names both ports and what stops")
{
  AlwaysServing health;
  const CameraRpcListenInput input{.address = "127.0.0.1:0",
                                   .tlsAddress = "192.0.2.1:0",
                                   .tls = pkiMaterial("camera"),
                                   .services = {&health}};
  const auto listener = startCameraRpc(input);
  CHECK(listener.server == nullptr);
  CHECK(cameraRpcFailure(input, listener).value_or("") ==
        "gRPC server failed to listen on 127.0.0.1:0 and 192.0.2.1:0; without "
        "it sync cannot pull the camera tables, guard cannot announce, "
        "listen, sound the alarm or the siren, or take a person crop, the llm "
        "cannot read the camera catalog or use the camera tools, and settings "
        "cannot reach camera's settings or module data");
}
