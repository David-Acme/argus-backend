#pragma once

#include <feature/voice/voice-rpc-service.hxx>
#include <feature/watch/watch-conversations.hxx>

#include <argus/voice/v1/voice.pb.h>

#include <memory>
#include <string>
#include <vector>

[[nodiscard]] argus::voice::v1::WatchConversation watchConversationOf(const ConversationSnapshot& snapshot);

[[nodiscard]] argus::voice::v1::ReplyWatchVisitorResponse watchReplyOf(const VisitorReplyResult& result);

class ConversationDirectory final : public VoiceWatchDirectory
{
public:
  explicit ConversationDirectory(const std::unique_ptr<ConversationManager>& manager) : manager_(manager) {}

  [[nodiscard]] std::vector<argus::voice::v1::WatchConversation> conversations(int64_t userId) const override;
  [[nodiscard]] bool end(int64_t userId, const std::string& conversationId) override;
  [[nodiscard]] argus::voice::v1::ReplyWatchVisitorResponse reply(const WatchReplyRequest& request) override;

private:
  const std::unique_ptr<ConversationManager>& manager_;
};
