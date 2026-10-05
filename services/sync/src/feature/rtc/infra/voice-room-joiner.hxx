#pragma once

#include <feature/rtc/infra/rtc-ports.hxx>
#include <voice/voice-client.hxx>

#include <memory>

class VoiceRoomJoiner final : public RtcVoiceJoiner
{
public:
  explicit VoiceRoomJoiner(std::shared_ptr<const VoiceClient> client);
  drogon::Task<bool> join(argus::voice::v1::RtcJoin join) const override;
  drogon::Task<bool> farewell(RtcFarewellInput input) const override;

private:
  std::shared_ptr<const VoiceClient> client_;
};
