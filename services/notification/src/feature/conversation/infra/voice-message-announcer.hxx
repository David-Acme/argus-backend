#pragma once

#include <feature/conversation/services/conversation-ports.hxx>
#include <voice/voice-client.hxx>

#include <memory>
#include <optional>

class VoiceMessageAnnouncer final : public MessageAnnouncer
{
public:
  explicit VoiceMessageAnnouncer(std::shared_ptr<const VoiceClient> client);

  [[nodiscard]] std::optional<bool> announce(const MessageAnnouncement& announcement) const override;

private:
  std::shared_ptr<const VoiceClient> client_;
};
