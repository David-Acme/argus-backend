#pragma once

#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <optional>
#include <string>
#include <vector>

struct VoiceRpcListenInput
{
  std::string address;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
  std::vector<grpc::Service*> services;
};

[[nodiscard]] argus::client::Listener
startVoiceRpc(const VoiceRpcListenInput& input);

[[nodiscard]] std::optional<std::string>
voiceRpcFailure(const VoiceRpcListenInput& input,
                const argus::client::Listener& listener);
