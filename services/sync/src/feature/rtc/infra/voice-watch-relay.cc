#include "voice-watch-relay.hxx"

#include <runtime/blocking-task.hxx>

#include <utility>

VoiceWatchRelay::VoiceWatchRelay(std::shared_ptr<const VoiceClient> client) : client_(std::move(client)) {}

drogon::Task<std::optional<std::vector<argus::voice::v1::WatchConversation>>>
VoiceWatchRelay::list(int64_t userId) const
{
  co_return co_await BlockingTask<std::optional<std::vector<argus::voice::v1::WatchConversation>>>{
      [client = client_, userId] { return client->watchConversations(userId); }};
}

drogon::Task<std::optional<bool>> VoiceWatchRelay::end(RtcWatchEnd input) const
{
  co_return co_await BlockingTask<std::optional<bool>>{
      [client = client_, request = std::move(input)] {
        return client->endWatchConversation(request.userId, request.conversationId);
      }};
}

drogon::Task<std::optional<argus::voice::v1::ReplyWatchVisitorResponse>> VoiceWatchRelay::reply(RtcWatchReply input) const
{
  co_return co_await BlockingTask<std::optional<argus::voice::v1::ReplyWatchVisitorResponse>>{
      [client = client_, request = std::move(input)] {
        return client->replyWatchVisitor(
            {.userId = request.userId, .conversationId = request.conversationId, .text = request.text});
      }};
}
