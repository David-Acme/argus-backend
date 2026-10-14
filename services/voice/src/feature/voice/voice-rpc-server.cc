#include "voice-rpc-server.hxx"

argus::client::Listener startVoiceRpc(const VoiceRpcListenInput& input)
{
  return argus::client::startListening({.address = input.address,
                                        .tlsAddress = input.tlsAddress,
                                        .tls = input.tls,
                                        .services = input.services});
}

std::optional<std::string>
voiceRpcFailure(const VoiceRpcListenInput& input,
                const argus::client::Listener& listener)
{
  if (listener.server)
    return std::nullopt;
  std::string where = input.address;
  if (input.tls && !input.tlsAddress.empty())
    where += " and " + input.tlsAddress;
  return "gRPC server failed to listen on " + where;
}
