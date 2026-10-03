#include "voice-config.hxx"

#include <config/config-service.hxx>
#include <settings/settings-rpc.hxx>

ListenerConfig VoiceConfig::resolveHealthListener()
{
  return ListenerConfig::resolve(7035, "server.health_port");
}

GrpcListenerConfig VoiceConfig::resolveGrpcListener()
{
  return GrpcListenerConfig::resolve(7034);
}

std::string VoiceConfig::resolveSyncCallerSecret()
{
  return ConfigService::getString("grpc.caller_sync");
}

std::vector<argus::client::CallerCredential> VoiceConfig::resolveSettingsCallers()
{
  const std::string secret = ConfigService::getString("grpc.caller_settings");
  if (secret == resolveSyncCallerSecret())
    return {};
  return settingsCallers({{kSettingsCaller, secret}});
}
