#include "auth-rpc-server.hxx"

namespace
{

argus::client::ListenInput listenInputOf(const AuthRpcListenInput& input)
{
  return {.address = input.address,
          .tlsAddress = input.tlsAddress,
          .tls = input.tls,
          .services = input.services,
          .maxReceiveBytes = std::nullopt};
}

}

argus::client::Listener startAuthRpc(const AuthRpcListenInput& input)
{
  return argus::client::startListening(listenInputOf(input));
}

std::optional<std::string>
authRpcFailure(const AuthRpcListenInput& input,
               const argus::client::Listener& listener)
{
  const auto listening = listenInputOf(input);
  return argus::client::listenFailure(
      {.name = "Auth RPC",
       .input = listening,
       .listener = listener,
       .consequence =
           "without it no service can validate a session against this one"});
}
