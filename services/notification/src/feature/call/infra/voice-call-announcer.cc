#include "voice-call-announcer.hxx"

#include <utility>

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
