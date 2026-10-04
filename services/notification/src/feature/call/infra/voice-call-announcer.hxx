#pragma once

#include <feature/call/services/call-ports.hxx>
#include <voice/voice-client.hxx>

#include <memory>

class VoiceCallAnnouncer final : public LiveCallAnnouncer
{
public:
  explicit VoiceCallAnnouncer(std::shared_ptr<const VoiceClient> client);

  [[nodiscard]] std::optional<bool>
  announce(const CallAnnouncement& input) const override;

private:
  std::shared_ptr<const VoiceClient> client_;
};
