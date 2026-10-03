#pragma once

#include <grpc/grpc-server-identity.hxx>
#include <http/listener-config.hxx>
#include <string>
#include <vector>

class VoiceConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveHealthListener();

  [[nodiscard]] static GrpcListenerConfig resolveGrpcListener();

  [[nodiscard]] static std::string resolveSyncCallerSecret();

  [[nodiscard]] static std::vector<argus::client::CallerCredential> resolveSettingsCallers();
};
