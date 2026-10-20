#pragma once

#include <feature/rtc/infra/rtc-ports.hxx>
#include <voice/voice-client.hxx>

#include <memory>

class VoiceWatchRelay final : public RtcWatchDirectory
{
public:
  explicit VoiceWatchRelay(std::shared_ptr<const VoiceClient> client);
  drogon::Task<std::optional<std::vector<argus::voice::v1::WatchConversation>>> list(int64_t userId) const override;
  drogon::Task<std::optional<bool>> end(RtcWatchEnd input) const override;
  drogon::Task<std::optional<argus::voice::v1::ReplyWatchVisitorResponse>> reply(RtcWatchReply input) const override;

private:
  std::shared_ptr<const VoiceClient> client_;
};
