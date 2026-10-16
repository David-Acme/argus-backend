#pragma once

#include <grpc-test-support/internal-tls-probe.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace argus::client::test_support
{

struct MutualTlsEndpointInput
{
  std::string service;
  std::vector<grpc::Service*> services;
};

class MutualTlsEndpoint
{
public:
  explicit MutualTlsEndpoint(const MutualTlsEndpointInput& input)
      : listener_(startListening({.address = "127.0.0.1:0",
                                  .tlsAddress = "127.0.0.1:0",
                                  .tls = pkiMaterial(input.service),
                                  .services = input.services,
                                  .maxReceiveBytes = std::nullopt}))
  {
    if (!listener_.server || listener_.tlsPort <= 0)
      throw std::runtime_error("the mutual TLS endpoint did not start");
  }

  [[nodiscard]] int plainPort() const { return listener_.port; }

  [[nodiscard]] int tlsPort() const { return listener_.tlsPort; }

  [[nodiscard]] std::string plainTarget() const
  {
    return "127.0.0.1:" + std::to_string(listener_.port);
  }

  [[nodiscard]] std::string tlsTarget() const
  {
    return std::string(kInternalTlsScheme) + "127.0.0.1:" +
           std::to_string(listener_.tlsPort);
  }

  [[nodiscard]] std::string plaintextToTlsPortTarget() const
  {
    return "127.0.0.1:" + std::to_string(listener_.tlsPort);
  }

private:
  Listener listener_;
};

}
