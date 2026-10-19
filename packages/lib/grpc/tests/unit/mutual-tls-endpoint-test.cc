#include <doctest/doctest.h>

#include <grpc-test-support/internal-tls-probe.hxx>
#include <grpc-test-support/mutual-tls-endpoint.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpc/internal-tls.hxx>

namespace
{

using argus::client::test_support::AlwaysServing;
using argus::client::test_support::MutualTlsEndpoint;
using argus::client::test_support::healthOver;
using argus::client::test_support::pkiMaterial;

}

TEST_CASE("a channel dials the mutual TLS port with the internal certificate")
{
  AlwaysServing service;
  const MutualTlsEndpoint endpoint({.service = "identity", .services = {&service}});
  REQUIRE(endpoint.tlsPort() > 0);
  argus::client::installInternalTls(pkiMaterial("camera"));
  CHECK(healthOver(argus::client::makeChannel(endpoint.tlsTarget())) ==
        grpc::StatusCode::OK);
}

TEST_CASE("a plaintext channel to the mutual TLS port is refused while the plain port serves")
{
  AlwaysServing service;
  const MutualTlsEndpoint endpoint({.service = "identity", .services = {&service}});
  REQUIRE(endpoint.tlsPort() > 0);
  CHECK(healthOver(argus::client::makeChannel(endpoint.plaintextToTlsPortTarget())) !=
        grpc::StatusCode::OK);
  CHECK(healthOver(argus::client::makeChannel(endpoint.plainTarget())) ==
        grpc::StatusCode::OK);
}
