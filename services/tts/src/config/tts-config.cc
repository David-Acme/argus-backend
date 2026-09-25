#include "tts-config.hxx"

#include <config/config-service.hxx>

#include <algorithm>

ListenerConfig TtsConfig::resolveListener()
{
  return ListenerConfig::resolve(7029);
}

TtsRpcConfig TtsConfig::resolveRpc()
{
  TtsRpcConfig config;
  config.address = ConfigService::getString("rpc.address");
  config.credentials = ConfigService::getStringPairs("rpc.callers");
  std::erase_if(config.credentials, [](const auto& credential) {
    return credential.first.empty() || credential.second.empty();
  });
  return config;
}
