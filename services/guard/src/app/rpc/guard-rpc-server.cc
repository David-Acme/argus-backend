#include "guard-rpc-server.hxx"

argus::client::Listener startGuardRpc(const GuardRpcListenInput& input)
{
  return argus::client::startListening({.address = input.address,
                                        .tlsAddress = input.tlsAddress,
                                        .tls = input.tls,
                                        .services = input.services});
}

std::optional<std::string>
guardRpcFailure(const GuardRpcListenInput& input,
                const argus::client::Listener& listener)
{
  if (listener.server)
    return std::nullopt;
  std::string where = input.address;
  if (input.tls && !input.tlsAddress.empty())
    where += " and " + input.tlsAddress;
  return "Guard RPC listener failed to listen on " + where;
}
