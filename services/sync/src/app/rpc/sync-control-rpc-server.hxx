#pragma once

#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <optional>
#include <string>
#include <vector>

struct SyncControlRpcListenInput
{
  std::string address;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
  std::vector<grpc::Service*> services;
};

[[nodiscard]] argus::client::Listener
startSyncControlRpc(const SyncControlRpcListenInput& input);

[[nodiscard]] std::optional<std::string>
syncControlRpcFailure(const SyncControlRpcListenInput& input,
                      const argus::client::Listener& listener);
