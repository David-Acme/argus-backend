#include "remote-config.hxx"

#include <shared/services/config-service/config-service.hxx>

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
