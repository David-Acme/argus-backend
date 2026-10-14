#include "sync-control-rpc-server.hxx"

namespace
{

argus::client::ListenInput listenInputOf(const SyncControlRpcListenInput& input)
{
  return {.address = input.address,
          .tlsAddress = input.tlsAddress,
          .tls = input.tls,
          .services = input.services,
          .maxReceiveBytes = std::nullopt};
}

}

argus::client::Listener
startSyncControlRpc(const SyncControlRpcListenInput& input)
{
  return argus::client::startListening(listenInputOf(input));
}

std::optional<std::string>
syncControlRpcFailure(const SyncControlRpcListenInput& input,
                      const argus::client::Listener& listener)
{
  const auto listening = listenInputOf(input);
  return argus::client::listenFailure(
      {.name = "Sync control RPC",
       .input = listening,
       .listener = listener,
       .consequence = "without it identity, notification and guard cannot "
                      "reach a user's room"});
}
