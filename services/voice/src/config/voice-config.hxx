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

enum class WakeStageOne : uint8_t
{
  Auto = 0,
  Kws,
  Stt
};

[[nodiscard]] inline std::string_view wakeStageOneToString(WakeStageOne stage)
{
  switch (stage) {
    case WakeStageOne::Auto:
      return "auto";
    case WakeStageOne::Kws:
      return "kws";
    case WakeStageOne::Stt:
      return "stt";
  }
  return "auto";
}

[[nodiscard]] inline std::optional<WakeStageOne> wakeStageOneFromString(std::string_view value)
{
  if (value == "auto")
    return WakeStageOne::Auto;
  if (value == "kws")
    return WakeStageOne::Kws;
  if (value == "stt")
    return WakeStageOne::Stt;
  return std::nullopt;
}

struct VoiceCameraConfig
{
  std::string target;
  std::string credential;
};

struct VoiceGuardConfig
{
  std::string target;
  std::string credential;
};

struct VoiceWatchConfig
{
  bool enabled{true};
  std::chrono::milliseconds listRefresh{10000};
  std::chrono::milliseconds reconnect{2000};
  WakeStageOne stageOne{WakeStageOne::Auto};
  std::chrono::milliseconds ring{2000};
  std::chrono::milliseconds postRoll{1000};
  int wakeMaxDistance{2};
  std::chrono::milliseconds sttStageOneMax{2500};
  std::chrono::milliseconds sttStageOneMin{300};
  float sttStageOneMinSnrDb{6.0F};
  std::chrono::milliseconds kwsHangover{400};
  float minWakeSnrDb{3.0F};
  std::chrono::milliseconds broadcastWindow{150};
  int broadcastMinCameras{3};
  float broadcastSpreadDb{6.0F};
  float tvRaiseDb{6.0F};
  std::chrono::milliseconds echoTail{800};
  float echoCorrelation{0.6F};
  float echoRaiseDb{6.0F};
  std::chrono::milliseconds echoMaxLag{1000};
  float floorRiseDbPerSecond{0.5F};
  float floorFallDbPerSecond{10.0F};
  int fireLogSize{200};
  std::chrono::milliseconds fireDebounce{1500};
  std::chrono::milliseconds kwsRetry{5000};
  std::chrono::milliseconds arbitrationWindow{400};
  int stageTwoCandidates{2};
  float weightWake{1.0F};
  float weightSnr{0.1F};
  float weightStageTwo{1.0F};
  float gainWeight{0.5F};
  float gainAlpha{0.1F};
  std::chrono::milliseconds endSilence{700};
  std::chrono::milliseconds incompleteHold{2000};
  std::chrono::milliseconds maxTurn{30000};
  std::chrono::milliseconds ackAfter{1500};
  std::chrono::milliseconds failAfter{8000};
  std::chrono::milliseconds idleClose{20000};
  std::chrono::milliseconds maxLength{600000};
  std::chrono::milliseconds resumable{180000};
  std::chrono::milliseconds replyGap{700};
  std::chrono::milliseconds cameraDrop{3000};
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

  [[nodiscard]] static VoiceCameraConfig resolveCamera();

  [[nodiscard]] static VoiceWatchConfig resolveWatch();

  [[nodiscard]] static VoiceGuardConfig resolveGuard();
};
