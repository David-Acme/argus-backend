#include "identity-rpc-server.hxx"

namespace
{

constexpr int kMaxRpcReceiveBytes = 12 * 1024 * 1024;

argus::client::ListenInput listenInputOf(const IdentityRpcListenInput& input)
{
  return {.address = input.address,
          .tlsAddress = input.tlsAddress,
          .tls = input.tls,
          .services = input.services,
          .maxReceiveBytes = kMaxRpcReceiveBytes};
}

}

argus::client::Listener startIdentityRpc(const IdentityRpcListenInput& input)
{
  return argus::client::startListening(listenInputOf(input));
}

std::optional<std::string>
identityRpcFailure(const IdentityRpcListenInput& input,
                   const argus::client::Listener& listener)
{
  const auto listening = listenInputOf(input);
  return argus::client::listenFailure(
      {.name = "Identity RPC",
       .input = listening,
       .listener = listener,
       .consequence = "without it auth, camera, guard, notification, the llm, "
                      "voice, sync and settings get no answer from identity"});
}
