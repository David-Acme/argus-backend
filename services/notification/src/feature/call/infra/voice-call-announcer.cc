#include "voice-call-announcer.hxx"

#include <feature/call/schemas/call/call-schema.hxx>
#include <argus/voice/v1/voice.pb.h>

#include <string>
#include <utility>

namespace
{
constexpr int kCloseRoomDeadlineMs = 1500;
}

VoiceCallAnnouncer::VoiceCallAnnouncer(std::shared_ptr<const VoiceClient> client)
    : client_(std::move(client))
{
}

std::optional<bool>
VoiceCallAnnouncer::announce(const CallAnnouncement& input) const
{
  if (!client_)
    return std::nullopt;
  return client_->announce({.userId = input.userId,
                            .text = input.text,
                            .kind = input.kind,
                            .callId = input.callId});
}

bool VoiceCallAnnouncer::closeRoom(const CallRoomClose& input) const
{
  if (!client_)
    return false;
  argus::voice::v1::RtcFarewell farewell;
  farewell.set_room("u" + std::to_string(input.userId) + "." + call_id::format(input.callId));
  farewell.set_reason(input.reason);
  return client_->farewell(farewell, kCloseRoomDeadlineMs);
}
