#pragma once

#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <optional>
#include <string>
#include <vector>

struct GuardRpcListenInput
{
  std::string address;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
  std::vector<grpc::Service*> services;
};

[[nodiscard]] argus::client::Listener
startGuardRpc(const GuardRpcListenInput& input);

[[nodiscard]] std::optional<std::string>
guardRpcFailure(const GuardRpcListenInput& input,
                const argus::client::Listener& listener);
