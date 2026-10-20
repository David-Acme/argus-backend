#include "notification-conversation-transcript.hxx"

#include <drogon/drogon.h>
#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <string>
#include <utility>

namespace
{
constexpr int64_t kDaySeconds = int64_t{24} * 3600;
}

argus::notification::v1::AppendTurnsRequest appendTurnsOf(const TranscriptExchange& exchange, int64_t expiresAt)
{
  argus::notification::v1::AppendTurnsRequest request;
  request.set_user_id(exchange.userId);
  if (exchange.leg)
    request.set_leg(conversationLegKindToString(*exchange.leg));
  request.set_expires_at(expiresAt);
  auto* said = request.add_turns();
  said->set_speaker(argus::notification::v1::TURN_SPEAKER_USER);
  said->set_body(exchange.userText);
  auto* answered = request.add_turns();
  answered->set_speaker(argus::notification::v1::TURN_SPEAKER_ASSISTANT);
  answered->set_body(exchange.reply);
  return request;
}

NotificationConversationTranscript::NotificationConversationTranscript(NotificationTranscriptInput input)
    : client_(std::move(input.client)), retentionDays_(std::move(input.retentionDays))
{
}

void NotificationConversationTranscript::record(const TranscriptExchange& exchange) const
{
  if (!client_ || !transcribable(exchange))
    return;
  const int64_t days = retentionDays_ ? retentionDays_() : 0;
  auto request = appendTurnsOf(exchange, days > 0 ? exchange.at + days * kDaySeconds : 0);
  request.set_command_id("turn-" + std::to_string(exchange.userId) + "-" + std::to_string(exchange.at) + "-" +
                         std::to_string(sequence_.fetch_add(1) + 1));
  drogon::app().getLoop()->queueInLoop([client = client_, request = std::move(request)]() {
    drogon::async_run([client, request]() -> drogon::Task<void> {
      const NotificationRpcOutcome outcome = co_await BlockingTask<NotificationRpcOutcome>(
          [client, request]() { return client->appendTurns(request); });
      if (outcome != NotificationRpcOutcome::Success)
        LOG_WARN << "argus-llm: the conversation turn was not mirrored to the app inbox";
      co_return;
    });
  });
}
