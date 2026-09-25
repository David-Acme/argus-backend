#include "stt-config.hxx"

#include <config/config-service.hxx>

#include <algorithm>

ListenerConfig SttConfig::resolveListener()
{
  return ListenerConfig::resolve(7030);
}

SttRpcConfig SttConfig::resolveRpc()
{
  SttRpcConfig config;
  config.address = ConfigService::getString("rpc.address");
  config.credentials = ConfigService::getStringPairs("rpc.callers");
  std::erase_if(config.credentials, [](const auto& credential) {
    return credential.first.empty() || credential.second.empty();
  });
  return config;
}
