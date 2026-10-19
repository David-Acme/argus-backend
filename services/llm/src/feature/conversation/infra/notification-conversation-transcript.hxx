#pragma once

#include <feature/conversation/services/conversation-transcript.hxx>
#include <notification/notification-client.hxx>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

struct NotificationTranscriptInput
{
  std::shared_ptr<const NotificationClient> client;
  std::function<int64_t()> retentionDays;
};

class NotificationConversationTranscript final : public ConversationTranscript
{
public:
  explicit NotificationConversationTranscript(NotificationTranscriptInput input);

  void record(const TranscriptExchange& exchange) const override;

private:
  std::shared_ptr<const NotificationClient> client_;
  std::function<int64_t()> retentionDays_;
  mutable std::atomic<int64_t> sequence_{0};
};

[[nodiscard]] argus::notification::v1::AppendTurnsRequest appendTurnsOf(const TranscriptExchange& exchange,
                                                                        int64_t expiresAt);
