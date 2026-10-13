#include "modules-rpc-server.hxx"

argus::client::Listener startModulesRpc(const ModulesRpcListenInput& input)
{
  return argus::client::startListening({.address = input.address,
                                        .tlsAddress = input.tlsAddress,
                                        .tls = input.tls,
                                        .services = input.services});
}
