#pragma once

#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <optional>
#include <string>
#include <vector>

struct AuthRpcListenInput
{
  std::string address;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
  std::vector<grpc::Service*> services;
};

[[nodiscard]] argus::client::Listener
startAuthRpc(const AuthRpcListenInput& input);

[[nodiscard]] std::optional<std::string>
authRpcFailure(const AuthRpcListenInput& input,
               const argus::client::Listener& listener);
