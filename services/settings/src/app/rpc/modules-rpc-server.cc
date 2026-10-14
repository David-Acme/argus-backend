#include "modules-rpc-server.hxx"

namespace
{

argus::client::ListenInput listenInputOf(const ModulesRpcListenInput& input)
{
  return {.address = input.address,
          .tlsAddress = input.tlsAddress,
          .tls = input.tls,
          .services = input.services,
          .maxReceiveBytes = std::nullopt};
}

}

argus::client::Listener startModulesRpc(const ModulesRpcListenInput& input)
{
  return argus::client::startListening(listenInputOf(input));
}

std::optional<std::string>
modulesRpcFailure(const ModulesRpcListenInput& input,
                  const argus::client::Listener& listener)
{
  const auto listening = listenInputOf(input);
  return argus::client::listenFailure(
      {.name = "Modules RPC",
       .input = listening,
       .listener = listener,
       .consequence = "without it camera, guard, identity, llm, notification, "
                      "productivity and sync read no enabled set at start and "
                      "keep their last known module state until the module "
                      "feed brings it, sync cannot read the Owner's catalog, "
                      "and the llm cannot use the modules tools"});
}
