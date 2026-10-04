#pragma once

#include <grpc/grpc-server-identity.hxx>
#include <http/listener-config.hxx>
#include <chrono>
#include <string>
#include <vector>

struct VoiceRtcConfig
{
  bool enabled{false};
  std::string url;
  std::chrono::milliseconds rejoinGrace{20000};
  std::chrono::milliseconds firstJoinWait{60000};
};

struct VoiceNotificationConfig
{
  std::string target;
  std::string credential;
};

class VoiceConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveHealthListener();

  [[nodiscard]] static GrpcListenerConfig resolveGrpcListener();

  [[nodiscard]] static std::string resolveSyncCallerSecret();

  [[nodiscard]] static std::vector<argus::client::CallerCredential> resolveSettingsCallers();

  [[nodiscard]] static std::string resolveNotificationCallerSecret();

  [[nodiscard]] static VoiceRtcConfig resolveRtc();

  [[nodiscard]] static VoiceNotificationConfig resolveNotification();
};
