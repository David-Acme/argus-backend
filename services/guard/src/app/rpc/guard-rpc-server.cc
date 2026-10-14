#include "guard-rpc-server.hxx"

namespace
{

argus::client::ListenInput listenInputOf(const GuardRpcListenInput& input)
{
  return {.address = input.address,
          .tlsAddress = input.tlsAddress,
          .tls = input.tls,
          .services = input.services,
          .maxReceiveBytes = std::nullopt};
}

}

argus::client::Listener startGuardRpc(const GuardRpcListenInput& input)
{
  return argus::client::startListening(listenInputOf(input));
}

std::optional<std::string>
guardRpcFailure(const GuardRpcListenInput& input,
                const argus::client::Listener& listener)
{
  const auto listening = listenInputOf(input);
  return argus::client::listenFailure(
      {.name = "Guard RPC listener",
       .input = listening,
       .listener = listener,
       .consequence =
           "without it sync cannot read presence, the llm cannot use the "
           "guard tools, read the duress phrase or raise a spoken duress, and "
           "settings cannot reach guard's settings, module data, owner PIN or "
           "module impact"});
}
