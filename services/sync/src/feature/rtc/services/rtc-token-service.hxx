#pragma once

#include <auth/jwt-filter.hxx>
#include <auth/user-directory.hxx>
#include <config/sync-config.hxx>
#include <feature/rtc/dtos/response-rtc-decline-dto.hxx>
#include <feature/rtc/dtos/response-rtc-ice-dto.hxx>
#include <feature/rtc/dtos/response-rtc-token-dto.hxx>
#include <feature/rtc/dtos/response-rtc-watch-dto.hxx>
#include <feature/rtc/dtos/rtc-watch-dto.hxx>
#include <feature/rtc/dtos/rtc-decline-dto.hxx>
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
  std::shared_ptr<const RtcCallDecliner> decliner;
  std::shared_ptr<const RtcWatchDirectory> watch{};
};

struct RtcWatchEndRequest
{
  RtcWatchEndDto body;
  JwtContext caller;
};

struct RtcWatchReplyRequest
{
  RtcWatchReplyDto body;
  JwtContext caller;
};

[[nodiscard]] bool watchExpired(const argus::voice::v1::WatchConversation& conversation, int64_t now);

struct RtcTokenRequest
{
  RtcTokenDto body;
  JwtContext caller;
  std::string host;
};

struct RtcDeclineRequest
{
  RtcDeclineDto body;
  JwtContext caller;
};

struct RtcIceRequest
{
  JwtContext caller;
  int64_t now{0};
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

  drogon::Task<ResponseRtcDeclineDto> decline(RtcDeclineRequest request) const;

  [[nodiscard]] ResponseRtcIceDto ice(const RtcIceRequest& request) const;

  drogon::Task<ResponseRtcWatchDto> watchConversations(JwtContext caller) const;

  drogon::Task<ResponseRtcWatchEndDto> endWatch(RtcWatchEndRequest request) const;

  drogon::Task<ResponseRtcWatchReplyDto> replyWatch(RtcWatchReplyRequest request) const;

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
  std::shared_ptr<const RtcCallDecliner> decliner_;
  std::shared_ptr<const RtcWatchDirectory> watch_;
  std::shared_ptr<InFlightCalls> inFlight_ = std::make_shared<InFlightCalls>();
};
