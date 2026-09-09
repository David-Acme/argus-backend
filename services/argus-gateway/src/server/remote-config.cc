#include "remote-config.hxx"

#include <shared/services/config-service/config-service.hxx>
#include <stdexcept>
#include <string>

RemoteConfig RemoteConfig::resolve()
{
  RemoteConfig config;
  const int port = ConfigService::getInt("remote.tunnel_port");
  if (port > 0 && port < 65536)
    config.tunnelPort = static_cast<uint16_t>(port);
  config.enabled = ConfigService::getBool("remote.enabled");
  return config;
}

bool requestIsRemote(const drogon::HttpRequestPtr& req,
                     const RemoteConfig& config)
{
  return config.tunnelPort != 0
         && req->localAddr().toPort() == config.tunnelPort;
}

void appendRemoteListener(Json::Value& listeners, const RemoteConfig& remote,
                          const ListenerConfig& base)
{
  if (remote.tunnelPort == 0)
    return;
  listeners.append(
      singleListenerJson(base, static_cast<int>(remote.tunnelPort)));
}

void requireDistinctTunnelPort(const ListenerConfig& listener,
                               const RemoteConfig& remote)
{
  if (remote.tunnelPort != 0 && remote.tunnelPort == listener.port)
    throw std::runtime_error("[remote] tunnel_port "
                             + std::to_string(remote.tunnelPort)
                             + " collides with [gateway] port "
                             + std::to_string(listener.port));
}
