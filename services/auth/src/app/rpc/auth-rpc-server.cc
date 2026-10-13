#include "auth-rpc-server.hxx"

argus::client::Listener startAuthRpc(const AuthRpcListenInput& input)
{
  return argus::client::startListening({.address = input.address,
                                        .tlsAddress = input.tlsAddress,
                                        .tls = input.tls,
                                        .services = input.services});
}

std::optional<std::string>
authRpcFailure(const AuthRpcListenInput& input,
               const argus::client::Listener& listener)
{
  if (listener.server)
    return std::nullopt;
  std::string where = input.address;
  if (input.tls && !input.tlsAddress.empty())
    where += " and " + input.tlsAddress;
  return "Auth RPC failed to listen on " + where +
         "; without it no service can validate a session against this one";
}
