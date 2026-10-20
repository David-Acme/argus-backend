#include "visitor-replies.hxx"

#include <algorithm>

namespace
{

int64_t seconds(std::chrono::milliseconds span)
{
  return std::chrono::duration_cast<std::chrono::seconds>(span).count();
}

}

std::string visitorReplyLine(VoiceLang lang, std::string_view text)
{
  if (lang == VoiceLang::En)
    return "I have a message from the household for you: " + std::string(text);
  return "Tengo un mensaje de la casa para usted: " + std::string(text);
}

int64_t utf8Characters(std::string_view text)
{
  return static_cast<int64_t>(
      std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}

int64_t VisitorReplies::open(const VisitorThreadInput& input)
{
  const int64_t until = input.at + seconds(config_.accept);
  std::scoped_lock lock(mutex_);
  purge(input.at);
  threads_[input.conversationId] = {.cameraId = input.cameraId,
                                    .personId = input.personId,
                                    .recipients = input.recipients,
                                    .acceptUntil = until};
  return until;
}

VisitorReplyResult VisitorReplies::reply(const VisitorReplyInput& input)
{
  std::scoped_lock lock(mutex_);
  const auto found = threads_.find(input.conversationId);
  if (found == threads_.end() || std::ranges::find(found->second.recipients, input.userId) == found->second.recipients.end())
    return {.outcome = VisitorReplyOutcome::NotFound, .replyUntil = 0, .cameraId = 0};
  const Thread& thread = found->second;
  if (input.now > thread.acceptUntil)
    return {.outcome = VisitorReplyOutcome::Expired, .replyUntil = 0, .cameraId = thread.cameraId};
  if (utf8Characters(input.text) > config_.maxChars)
    return {.outcome = VisitorReplyOutcome::TooLong, .replyUntil = 0, .cameraId = thread.cameraId};
  const int64_t until = input.now + seconds(config_.hold);
  pending_[thread.cameraId] = {.text = input.text,
                               .personId = thread.personId,
                               .until = until,
                               .conversationId = input.conversationId};
  return {.outcome = VisitorReplyOutcome::Queued, .replyUntil = until, .cameraId = thread.cameraId};
}

std::optional<PendingVisitorReply> VisitorReplies::take(const VisitorReplyTake& input)
{
  std::scoped_lock lock(mutex_);
  purge(input.now);
  const auto found = pending_.find(input.cameraId);
  if (found == pending_.end())
    return std::nullopt;
  const PendingVisitorReply& pending = found->second;
  if (pending.personId > 0 && input.personId > 0 && pending.personId != input.personId)
    return std::nullopt;
  PendingVisitorReply taken = pending;
  pending_.erase(found);
  return taken;
}

size_t VisitorReplies::pendingCount() const
{
  std::scoped_lock lock(mutex_);
  return pending_.size();
}

void VisitorReplies::purge(int64_t now)
{
  std::erase_if(pending_, [now](const auto& entry) { return now > entry.second.until; });
  const int64_t grace = seconds(config_.accept);
  std::erase_if(threads_, [now, grace](const auto& entry) { return now > entry.second.acceptUntil + grace; });
}
