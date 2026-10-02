#pragma once

#include <http/listener-config.hxx>
#include <string>

class VoiceConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveHealthListener();

  [[nodiscard]] static GrpcListenerConfig resolveGrpcListener();

  [[nodiscard]] static std::string resolveSyncCallerSecret();
};
