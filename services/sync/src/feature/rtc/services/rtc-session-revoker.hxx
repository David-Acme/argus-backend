#pragma once

#include <feature/rtc/infra/livekit-room-client.hxx>

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct RtcSessionEnd
{
  int64_t userId{0};
  std::optional<std::string> sessionId;
};

class RtcSessionRevoker
{
public:
  explicit RtcSessionRevoker(std::shared_ptr<const LiveKitRoomClient> rooms);

  void sessionEnded(RtcSessionEnd end) const;

  static drogon::Task<int> revoke(std::shared_ptr<const LiveKitRoomClient> rooms, RtcSessionEnd end);

private:
  std::shared_ptr<const LiveKitRoomClient> rooms_;
};
