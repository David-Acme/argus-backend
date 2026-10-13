#include "sync-control-rpc-server.hxx"

argus::client::Listener
startSyncControlRpc(const SyncControlRpcListenInput& input)
{
  return argus::client::startListening({.address = input.address,
                                        .tlsAddress = input.tlsAddress,
                                        .tls = input.tls,
                                        .services = input.services});
}

std::optional<std::string>
syncControlRpcFailure(const SyncControlRpcListenInput& input,
                      const argus::client::Listener& listener)
{
  if (listener.server)
    return std::nullopt;
  std::string where = input.address;
  if (input.tls && !input.tlsAddress.empty())
    where += " and " + input.tlsAddress;
  return "Sync control RPC failed to listen on " + where +
         "; without it identity, notification and guard cannot reach a "
         "user's room";
}
