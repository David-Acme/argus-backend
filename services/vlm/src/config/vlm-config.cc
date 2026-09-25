#include "vlm-config.hxx"

#include <config/config-service.hxx>

#include <algorithm>

ListenerConfig VlmConfig::resolveListener()
{
  return ListenerConfig::resolve(7031);
}

VlmRpcConfig VlmConfig::resolveRpc()
{
  VlmRpcConfig config;
  config.address = ConfigService::getString("rpc.address");
  config.credentials = ConfigService::getStringPairs("rpc.callers");
  std::erase_if(config.credentials, [](const auto& credential) {
    return credential.first.empty() || credential.second.empty();
  });
  return config;
}
