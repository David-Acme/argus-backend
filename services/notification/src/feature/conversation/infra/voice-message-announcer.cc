#include "voice-message-announcer.hxx"

#include <utility>

namespace
{
constexpr const char* kAnnouncementKind = "household_message";
}

VoiceMessageAnnouncer::VoiceMessageAnnouncer(std::shared_ptr<const VoiceClient> client)
    : client_(std::move(client))
{
}

std::optional<bool> VoiceMessageAnnouncer::announce(const MessageAnnouncement& announcement) const
{
  if (!client_)
    return std::nullopt;
  return client_->announce(
      {.userId = announcement.userId, .text = announcement.text, .kind = kAnnouncementKind, .callId = {}});
}
