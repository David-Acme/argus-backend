#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/modules-rpc-server.hxx>
#include <chrono>
#include <grpc-test-support/internal-tls-probe.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace
{

using argus::client::InternalTlsMaterial;
using argus::client::test_support::AlwaysServing;
using argus::client::test_support::foreignMaterial;
using argus::client::test_support::healthOver;
using argus::client::test_support::pkiMaterial;

std::shared_ptr<grpc::Channel>
channelTo(int port,
          const std::shared_ptr<grpc::ChannelCredentials>& credentials)
{
  return argus::client::test_support::loopbackChannel(
      {.port = port,
       .credentials = credentials,
       .serverName = "argus-settings"});
}

ModulesRpcListenInput listenInput(std::optional<InternalTlsMaterial> tls,
                                  std::string tlsAddress)
{
  return {.address = "127.0.0.1:0",
          .tlsAddress = std::move(tlsAddress),
          .tls = std::move(tls),
          .services = {}};
}

}

TEST_CASE("material and an address give a plaintext port and a mutual TLS port")
{
  const auto ports =
      modulesRpcPorts(listenInput(pkiMaterial("settings"), "127.0.0.1:7247"));
  REQUIRE(ports.size() == 2);
  CHECK(ports[0].address == "127.0.0.1:0");
  CHECK_FALSE(ports[0].mutualTls);
  CHECK(ports[0].credentials != nullptr);
  CHECK(ports[1].address == "127.0.0.1:7247");
  CHECK(ports[1].mutualTls);
  CHECK(ports[1].credentials != nullptr);
  CHECK(ports[1].credentials != ports[0].credentials);
}

TEST_CASE("without material there is a plaintext port only")
{
  const auto ports = modulesRpcPorts(listenInput(std::nullopt, ""));
  REQUIRE(ports.size() == 1);
  CHECK_FALSE(ports[0].mutualTls);
}

TEST_CASE(
    "an address without material is a plaintext port only, as the rule says")
{
  const auto ports =
      modulesRpcPorts(listenInput(std::nullopt, "127.0.0.1:7247"));
  REQUIRE(ports.size() == 1);
  CHECK(ports[0].address == "127.0.0.1:0");
  CHECK_FALSE(ports[0].mutualTls);
}

TEST_CASE(
    "material without an address loads it for clients and opens no second port")
{
  const auto ports = modulesRpcPorts(listenInput(pkiMaterial("settings"), ""));
  REQUIRE(ports.size() == 1);
  CHECK_FALSE(ports[0].mutualTls);
}

TEST_CASE("no plaintext address opens nothing, mutual TLS included")
{
  auto input = listenInput(pkiMaterial("settings"), "127.0.0.1:0");
  input.address.clear();
  CHECK(modulesRpcPorts(input).empty());
  const auto listener = startModulesRpc(input);
  CHECK(listener.server == nullptr);
  CHECK(listener.port == 0);
  CHECK(listener.tlsPort == 0);
}

TEST_CASE("the mutual TLS port serves a peer of the internal CA and refuses "
          "every other")
{
  AlwaysServing health;
  auto input = listenInput(pkiMaterial("settings"), "127.0.0.1:0");
  input.services = {&health};
  auto listener = startModulesRpc(input);
  REQUIRE(listener.server != nullptr);
  REQUIRE(listener.port > 0);
  REQUIRE(listener.tlsPort > 0);
  CHECK(listener.tlsPort != listener.port);

  CHECK(healthOver(
            channelTo(listener.port, grpc::InsecureChannelCredentials())) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(channelTo(listener.tlsPort,
                             argus::client::internalChannelCredentials(
                                 pkiMaterial("camera")))) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(channelTo(listener.tlsPort,
                             argus::client::internalChannelCredentials(
                                 foreignMaterial()))) ==
        grpc::StatusCode::UNAVAILABLE);
  const auto camera = pkiMaterial("camera");
  CHECK(healthOver(channelTo(listener.tlsPort,
                             argus::client::test_support::anonymousTls(
                                 camera))) == grpc::StatusCode::UNAVAILABLE);
  CHECK(healthOver(
            channelTo(listener.tlsPort, grpc::InsecureChannelCredentials())) ==
        grpc::StatusCode::UNAVAILABLE);
  listener.server->Shutdown(std::chrono::system_clock::now() +
                            std::chrono::seconds(2));
}

TEST_CASE("a mutual TLS address that cannot be bound starts nothing")
{
  AlwaysServing health;
  auto input = listenInput(pkiMaterial("settings"), "192.0.2.1:0");
  input.services = {&health};
  const auto listener = startModulesRpc(input);
  CHECK(listener.server == nullptr);
  CHECK(listener.port == 0);
  CHECK(listener.tlsPort == 0);
}
