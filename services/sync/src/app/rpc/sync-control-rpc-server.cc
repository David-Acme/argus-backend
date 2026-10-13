#include "sync-control-rpc-server.hxx"

argus::client::Listener
startSyncControlRpc(const SyncControlRpcListenInput& input)
{
  return argus::client::startListening({.address = input.address,
                                        .tlsAddress = input.tlsAddress,
                                        .tls = input.tls,
                                        .services = input.services});
}
