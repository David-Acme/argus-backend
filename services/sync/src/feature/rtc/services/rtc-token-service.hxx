#pragma once

#include <auth/jwt-filter.hxx>
#include <auth/user-directory.hxx>
#include <config/sync-config.hxx>
#include <feature/rtc/dtos/response-rtc-token-dto.hxx>
#include <feature/rtc/dtos/rtc-token-dto.hxx>
#include <feature/rtc/infra/livekit-room-client.hxx>
#include <feature/rtc/infra/rtc-ports.hxx>

#include <drogon/utils/coroutine.h>

#include <memory>
#include <string>

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

class RtcTokenService
{
public:
  explicit RtcTokenService(RtcTokenServiceInput input);

  drogon::Task<ResponseRtcTokenDto> issue(RtcTokenRequest request) const;

private:
  SyncRtcConfig config_;
  std::shared_ptr<const RtcVoiceJoiner> voice_;
  std::shared_ptr<const RtcCallClaimer> calls_;
  std::shared_ptr<const IUserDirectory> directory_;
  std::shared_ptr<const LiveKitRoomClient> rooms_;
};
