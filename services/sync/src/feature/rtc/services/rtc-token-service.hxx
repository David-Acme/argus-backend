#pragma once

#include <auth/jwt-filter.hxx>
#include <auth/user-directory.hxx>
#include <config/sync-config.hxx>
#include <feature/rtc/dtos/response-rtc-token-dto.hxx>
#include <feature/rtc/dtos/rtc-token-dto.hxx>
#include <feature/rtc/infra/livekit-room-client.hxx>
#include <feature/rtc/infra/rtc-ports.hxx>

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

struct RtcTokenServiceInput
{
  SyncRtcConfig config;
  std::shared_ptr<const RtcVoiceJoiner> voice;
  std::shared_ptr<const RtcCallClaimer> calls;
  std::shared_ptr<const IUserDirectory> directory;
  std::shared_ptr<const LiveKitRoomClient> rooms;
};

struct RtcTokenRequest
{
  RtcTokenDto body;
  JwtContext caller;
  std::string host;
};

struct RtcCallSlotInput
{
  int64_t userId{0};
  std::string room;
};

class RtcTokenService
{
public:
  explicit RtcTokenService(RtcTokenServiceInput input);

  drogon::Task<ResponseRtcTokenDto> issue(RtcTokenRequest request) const;

private:
  struct InFlightCalls
  {
    std::mutex mutex;
    std::unordered_map<int64_t, int> byUser;
  };

  struct CallSlot
  {
    std::shared_ptr<void> release;
  };

  drogon::Task<CallSlot> reserveCall(RtcCallSlotInput input) const;

  SyncRtcConfig config_;
  std::shared_ptr<const RtcVoiceJoiner> voice_;
  std::shared_ptr<const RtcCallClaimer> calls_;
  std::shared_ptr<const IUserDirectory> directory_;
  std::shared_ptr<const LiveKitRoomClient> rooms_;
  std::shared_ptr<InFlightCalls> inFlight_ = std::make_shared<InFlightCalls>();
};
