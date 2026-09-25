#pragma once

#include <http/listener-config.hxx>

class VoiceConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveHealthListener();

  [[nodiscard]] static GrpcListenerConfig resolveGrpcListener();
};
