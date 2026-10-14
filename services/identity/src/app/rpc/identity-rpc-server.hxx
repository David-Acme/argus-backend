#pragma once

#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <optional>
#include <string>
#include <vector>

struct IdentityRpcListenInput
{
  std::string address;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
  std::vector<grpc::Service*> services;
};

[[nodiscard]] argus::client::Listener
startIdentityRpc(const IdentityRpcListenInput& input);

[[nodiscard]] std::optional<std::string>
identityRpcFailure(const IdentityRpcListenInput& input,
                   const argus::client::Listener& listener);
