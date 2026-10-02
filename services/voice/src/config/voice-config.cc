#include "voice-config.hxx"

#include <config/config-service.hxx>

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
