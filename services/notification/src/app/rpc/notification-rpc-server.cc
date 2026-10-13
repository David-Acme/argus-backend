#include "notification-rpc-server.hxx"

argus::client::Listener
startNotificationRpc(const NotificationRpcListenInput& input)
{
  return argus::client::startListening({.address = input.address,
                                        .tlsAddress = input.tlsAddress,
                                        .tls = input.tls,
                                        .services = input.services});
}
