#pragma once

#include <feature/rtc/infra/livekit-room-client.hxx>
#include <feature/rtc/infra/rtc-ports.hxx>

#include <drogon/utils/coroutine.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct RtcSessionEnd
{
  int64_t userId{0};
  std::optional<std::string> sessionId;
  std::string cause;
};

struct RtcRevokeInput
{
  std::shared_ptr<const LiveKitRoomClient> rooms;
  std::shared_ptr<const RtcVoiceJoiner> voice;
  RtcSessionEnd end;
  std::chrono::milliseconds farewellBudget{2400};
  std::vector<std::chrono::milliseconds> listRetries;
};

class RtcSessionRevoker
{
public:
  RtcSessionRevoker(std::shared_ptr<const LiveKitRoomClient> rooms, std::shared_ptr<const RtcVoiceJoiner> voice);

  void sessionEnded(RtcSessionEnd end) const;

  static drogon::Task<int> revoke(RtcRevokeInput input);

  static constexpr auto kFarewellBudget = std::chrono::milliseconds(2400);
  static std::vector<std::chrono::milliseconds> defaultListRetries();

private:
  std::shared_ptr<const LiveKitRoomClient> rooms_;
  std::shared_ptr<const RtcVoiceJoiner> voice_;
};
