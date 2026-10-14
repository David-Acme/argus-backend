#include "notification-rpc-server.hxx"

namespace
{

argus::client::ListenInput
listenInputOf(const NotificationRpcListenInput& input)
{
  return {.address = input.address,
          .tlsAddress = input.tlsAddress,
          .tls = input.tls,
          .services = input.services,
          .maxReceiveBytes = std::nullopt};
}

}

argus::client::Listener
startNotificationRpc(const NotificationRpcListenInput& input)
{
  return argus::client::startListening(listenInputOf(input));
}

std::optional<std::string>
notificationRpcFailure(const NotificationRpcListenInput& input,
                       const argus::client::Listener& listener)
{
  const auto listening = listenInputOf(input);
  return argus::client::listenFailure(
      {.name = "gRPC server",
       .input = listening,
       .listener = listener,
       .consequence =
           "without it guard cannot notify a user, productivity cannot "
           "announce the agenda, the llm cannot schedule a reminder call or "
           "post a notice, voice cannot report the end of a call, sync cannot "
           "pull notifications or claim a call, and settings cannot reach "
           "notification's settings or module requests"});
}
