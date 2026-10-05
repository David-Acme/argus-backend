#include "rtc-token-service.hxx"

#include <feature/rtc/rtc-errors.hxx>
#include <feature/rtc/services/livekit-token.hxx>
#include <feature/rtc/services/rtc-naming.hxx>

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <voice/voice-client.hxx>

#include <chrono>
#include <optional>
#include <utility>

namespace
{

void throwForClaim(RtcClaimStatus status)
{
  switch (status) {
    case RtcClaimStatus::Claimed:
      return;
    case RtcClaimStatus::Taken:
      throw ResponseException(RtcErrors::CallTaken);
    case RtcClaimStatus::Expired:
      throw ResponseException(RtcErrors::CallExpired);
    case RtcClaimStatus::NotFound:
      throw ResponseException(RtcErrors::CallNotFound);
    case RtcClaimStatus::Unavailable:
      break;
  }
  throw ResponseException(RtcErrors::RtcUnavailable);
}

}

RtcTokenService::RtcTokenService(RtcTokenServiceInput input)
    : config_(std::move(input.config)),
      voice_(std::move(input.voice)),
      calls_(std::move(input.calls)),
      directory_(std::move(input.directory)),
      rooms_(std::move(input.rooms))
{
}

drogon::Task<ResponseRtcTokenDto> RtcTokenService::issue(RtcTokenRequest request) const
{
  const JwtContext& caller = request.caller;
  if (!config_.enabled || !voice_ || caller.sessionId.empty())
    throw ResponseException(RtcErrors::RtcUnavailable);

  std::string callId = request.body.callId.empty() ? rtc_naming::mintUserCallId() : request.body.callId;
  const rtc_naming::CallKind kind = rtc_naming::callKindOf(callId);
  std::optional<RtcClaim> claim;
  if (kind == rtc_naming::CallKind::Proactive) {
    if (!calls_)
      throw ResponseException(RtcErrors::RtcUnavailable);
    claim = co_await calls_->claim(
        {.callId = callId, .userId = caller.sub, .sessionId = caller.sessionId});
    throwForClaim(claim->status);
  }

  std::optional<DirectoryUser> user;
  if (directory_)
    user = co_await directory_->findById(caller.sub);

  const std::string room = rtc_naming::roomOf(caller.sub, callId);
  const std::string identity = rtc_naming::userIdentityOf(caller.sub, caller.sessionId);
  const auto now = std::chrono::system_clock::now();

  argus::voice::v1::RtcJoin join;
  join.set_room(room);
  join.set_agent_token(mintLiveKitToken({.apiKey = config_.apiKey,
                                         .apiSecret = config_.apiSecret,
                                         .identity = std::string(rtc_naming::kAgentIdentity),
                                         .kind = "agent",
                                         .grant = {.room = room,
                                                   .roomJoin = true,
                                                   .roomAdmin = false,
                                                   .roomList = false,
                                               .roomCreate = false,
                                                   .canPublish = true,
                                                   .publishSources = {"microphone"},
                                                   .canSubscribe = true,
                                                   .canPublishData = true,
                                                   .canUpdateOwnMetadata = true},
                                         .ttl = config_.tokenTtl,
                                         .now = now}));
  join.set_user_identity(identity);
  auto& voiceIdentity = *join.mutable_identity();
  voiceIdentity.set_user_id(caller.sub);
  voiceIdentity.set_role(voiceRoleToProto(caller.role));
  voiceIdentity.set_device_hash(caller.deviceHash);
  if (user) {
    voiceIdentity.set_name(user->name);
    voiceIdentity.set_language(voiceLanguageToProto(user->lang));
  }
  if (claim && !claim->lang.empty())
    voiceIdentity.set_language(voiceLanguageToProto(claim->lang));
  join.set_mode(request.body.mode == "half" ? argus::voice::v1::VOICE_MODE_HALF_DUPLEX
                                            : argus::voice::v1::VOICE_MODE_DUPLEX);
  join.set_resume(request.body.resume);
  join.set_call_id(callId);
  if (claim) {
    join.set_opening_line(claim->openingLine);
    join.set_call_kind(claim->kind);
  }

  if (rooms_ && !co_await rooms_->createRoom(room))
    throw ResponseException(RtcErrors::RtcUnavailable);
  if (!co_await voice_->join(std::move(join)))
    throw ResponseException(RtcErrors::RtcUnavailable);

  ResponseRtcTokenDto response;
  response.url = rtc_naming::publicUrlOf(
      {.configured = config_.publicUrl, .host = request.host, .port = config_.publicPort});
  response.token = mintLiveKitToken({.apiKey = config_.apiKey,
                                     .apiSecret = config_.apiSecret,
                                     .identity = identity,
                                     .kind = {},
                                     .grant = {.room = room,
                                               .roomJoin = true,
                                               .roomAdmin = false,
                                               .roomList = false,
                                               .roomCreate = false,
                                               .canPublish = true,
                                               .publishSources = {"microphone"},
                                               .canSubscribe = true,
                                               .canPublishData = true,
                                               .canUpdateOwnMetadata = false},
                                     .ttl = config_.tokenTtl,
                                     .now = now});
  response.room = room;
  response.identity = identity;
  response.agentIdentity = std::string(rtc_naming::kAgentIdentity);
  response.callId = std::move(callId);
  response.expiresAt =
      std::chrono::duration_cast<std::chrono::seconds>((now + config_.tokenTtl).time_since_epoch()).count();
  if (claim)
    response.call = ResponseRtcCallDto{.kind = claim->kind,
                                       .summary = claim->summary,
                                       .lang = claim->lang,
                                       .cameraId = claim->cameraId,
                                       .cameraName = claim->cameraName,
                                       .episodeId = claim->episodeId};
  LOG_INFO << "RTC: token for user " << caller.sub << " room " << room
           << (request.body.resume ? " (resume)" : "");
  co_return response;
}
