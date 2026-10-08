#pragma once

#include <grpc/grpc-server-identity.hxx>
#include <http/listener-config.hxx>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class VoiceOpening : uint8_t
{
  None = 0,
  Spoken
};

[[nodiscard]] inline std::string_view voiceOpeningToString(VoiceOpening opening)
{
  switch (opening) {
    case VoiceOpening::None:
      return "none";
    case VoiceOpening::Spoken:
      return "spoken";
  }
  return "none";
}

[[nodiscard]] inline std::optional<VoiceOpening> voiceOpeningFromString(std::string_view value)
{
  if (value == "none")
    return VoiceOpening::None;
  if (value == "spoken")
    return VoiceOpening::Spoken;
  return std::nullopt;
}

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

  [[nodiscard]] static VoiceOpening resolveOpening();

  [[nodiscard]] static bool resolveLatencyTrace();

  [[nodiscard]] static VoiceNotificationConfig resolveNotification();
};
