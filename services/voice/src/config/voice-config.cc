#include "voice-config.hxx"

ListenerConfig VoiceConfig::resolveHealthListener()
{
  return ListenerConfig::resolve(7035, "server.health_port");
}

GrpcListenerConfig VoiceConfig::resolveGrpcListener()
{
  return GrpcListenerConfig::resolve(7034);
}
