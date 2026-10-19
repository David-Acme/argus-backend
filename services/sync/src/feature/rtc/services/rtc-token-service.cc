#include "rtc-token-service.hxx"

#include <feature/rtc/rtc-errors.hxx>
#include <feature/rtc/services/livekit-token.hxx>
#include <feature/rtc/services/rtc-naming.hxx>
#include <feature/rtc/services/turn-credentials.hxx>

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <voice/voice-client.hxx>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace
{

void throwForDecline(RtcDeclineStatus status)
{
  switch (status) {
    case RtcDeclineStatus::Declined:
      return;
    case RtcDeclineStatus::Taken:
      throw ResponseException(RtcErrors::CallTaken);
    case RtcDeclineStatus::Expired:
      throw ResponseException(RtcErrors::CallExpired);
    case RtcDeclineStatus::NotFound:
      throw ResponseException(RtcErrors::CallNotFound);
    case RtcDeclineStatus::Unavailable:
      break;
  }
  throw ResponseException(RtcErrors::RtcUnavailable);
}

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
      rooms_(std::move(input.rooms)),
      decliner_(std::move(input.decliner)),
      watch_(std::move(input.watch))
{
}

bool watchExpired(const argus::voice::v1::WatchConversation& conversation, int64_t now)
{
  if (conversation.state() == "ended")
    return true;
  return conversation.state() == "resumable" && conversation.resumable_until() > 0 &&
         conversation.resumable_until() < now;
}

drogon::Task<ResponseRtcWatchDto> RtcTokenService::watchConversations(JwtContext caller) const
{
  if (!watch_)
    throw ResponseException(RtcErrors::RtcUnavailable);
  const auto listed = co_await watch_->list(caller.sub);
  if (!listed)
    throw ResponseException(RtcErrors::RtcUnavailable);
  co_return ResponseRtcWatchDto{.conversations = *listed};
}

drogon::Task<ResponseRtcWatchEndDto> RtcTokenService::endWatch(RtcWatchEndRequest request) const
{
  if (!watch_)
    throw ResponseException(RtcErrors::RtcUnavailable);
  const auto ended = co_await watch_->end({.userId = request.caller.sub, .conversationId = request.body.conversationId});
  if (!ended)
    throw ResponseException(RtcErrors::RtcUnavailable);
  if (!*ended)
    throw ResponseException(RtcErrors::WatchNotFound);
  co_return ResponseRtcWatchEndDto{.ended = true};
}

drogon::Task<RtcTokenService::CallSlot>
RtcTokenService::reserveCall(RtcCallSlotInput input) const
{
  if (!rooms_)
    co_return CallSlot{};
  const auto names = co_await rooms_->listRooms();
  if (!names)
    throw ResponseException(RtcErrors::RtcUnavailable);
  const std::string prefix = rtc_naming::roomPrefixOf(input.userId);
  const auto open = std::ranges::count_if(*names, [&](const std::string& name) {
    return name.starts_with(prefix) && name != input.room;
  });
  const std::scoped_lock lock(inFlight_->mutex);
  int& pending = inFlight_->byUser[input.userId];
  if (open + pending >= config_.maxConcurrentCalls) {
    if (pending == 0)
      inFlight_->byUser.erase(input.userId);
    throw ResponseException(RtcErrors::TooManyCalls);
  }
  ++pending;
  co_return CallSlot{.release = std::shared_ptr<void>(
                         nullptr, [calls = inFlight_, userId = input.userId](void*) {
                           const std::scoped_lock release(calls->mutex);
                           const auto found = calls->byUser.find(userId);
                           if (found != calls->byUser.end() && --found->second <= 0)
                             calls->byUser.erase(found);
                         })};
}

drogon::Task<ResponseRtcDeclineDto> RtcTokenService::decline(RtcDeclineRequest request) const
{
  if (!config_.enabled || !decliner_ || request.caller.sessionId.empty())
    throw ResponseException(RtcErrors::RtcUnavailable);
  const RtcDecline result = co_await decliner_->decline(
      {.callId = request.body.callId, .userId = request.caller.sub, .sessionId = request.caller.sessionId});
  throwForDecline(result.status);
  LOG_INFO << "RTC: call " << request.body.callId << " declined by user " << request.caller.sub;
  co_return ResponseRtcDeclineDto{};
}

ResponseRtcIceDto RtcTokenService::ice(const RtcIceRequest& request) const
{
  const std::string subject = std::to_string(request.caller.sub);
  IceServers servers =
      turn_credentials::iceServers({.relay = config_.turn, .subject = subject, .now = request.now});
  return {.iceServers = std::move(servers.servers), .expiresAt = servers.expiresAt};
}

drogon::Task<ResponseRtcTokenDto> RtcTokenService::issue(RtcTokenRequest request) const
{
  const JwtContext& caller = request.caller;
  if (!config_.enabled || !voice_ || caller.sessionId.empty())
    throw ResponseException(RtcErrors::RtcUnavailable);

  const std::string& watchId = request.body.watchConversationId;
  if (!watchId.empty()) {
    if (!watch_)
      throw ResponseException(RtcErrors::RtcUnavailable);
    const auto listed = co_await watch_->list(caller.sub);
    if (!listed)
      throw ResponseException(RtcErrors::RtcUnavailable);
    const auto found = std::ranges::find_if(
        *listed, [&watchId](const argus::voice::v1::WatchConversation& item) { return item.conversation_id() == watchId; });
    if (found == listed->end())
      throw ResponseException(RtcErrors::WatchNotFound);
    const int64_t now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (watchExpired(*found, now))
      throw ResponseException(RtcErrors::WatchExpired);
  }
  std::string callId = !watchId.empty()               ? std::string(rtc_naming::kUserCallPrefix) + watchId
                       : request.body.callId.empty() ? rtc_naming::mintUserCallId()
                                                      : request.body.callId;
  const rtc_naming::CallKind kind = rtc_naming::callKindOf(callId);
  const std::string room = rtc_naming::roomOf(caller.sub, callId);
  const CallSlot slot = co_await reserveCall({.userId = caller.sub, .room = room});
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
  if (!watchId.empty()) {
    join.set_watch_conversation_id(watchId);
    join.set_call_kind("watch");
  }
  if (claim) {
    join.set_opening_line(claim->openingLine);
    for (const auto& line : claim->laterLines)
      join.add_later_lines(line);
    join.set_call_kind(claim->kind);
  }

  if (rooms_ && !co_await rooms_->createRoom(room))
    throw ResponseException(RtcErrors::RtcUnavailable);
  if (!co_await voice_->join(std::move(join)))
    throw ResponseException(watchId.empty() ? RtcErrors::RtcUnavailable : RtcErrors::WatchExpired);

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
  response.iceServers =
      turn_credentials::iceServers(
          {.relay = config_.turn,
           .subject = std::to_string(caller.sub),
           .now = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()})
          .servers;
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
