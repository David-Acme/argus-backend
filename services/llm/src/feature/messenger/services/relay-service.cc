#include "relay-service.hxx"

#include <errors/response-exception.hxx>
#include <feature/messenger/services/relay-lines.hxx>
#include <feature/messenger/vocabulary/messenger-errors.hxx>

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr int64_t kHourSeconds = 3600;
constexpr int64_t kDaySeconds = 24 * 3600;
constexpr int64_t kRetentionSeconds = kDaySeconds;

bool blank(std::string_view text)
{
  return std::ranges::all_of(text, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
}

int64_t characterCount(std::string_view text)
{
  return static_cast<int64_t>(std::ranges::count_if(
      text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}

std::string nameOf(const std::vector<relay_recipient::Member>& roster, int64_t userId)
{
  const auto member = std::ranges::find(roster, userId, &relay_recipient::Member::userId);
  return member == roster.end() ? std::string() : member->name;
}

std::vector<relay_recipient::Member> sendersOf(const std::vector<RelayThreadRow>& threads,
                                               const std::vector<relay_recipient::Member>& roster)
{
  std::vector<relay_recipient::Member> senders;
  for (const auto& member : roster) {
    const bool sent = std::ranges::any_of(threads, [&member](const RelayThreadRow& thread) {
      return thread.fromUserId == member.userId;
    });
    if (sent)
      senders.push_back(member);
  }
  return senders;
}

std::vector<std::string> senderNamesOf(const std::vector<RelayThreadRow>& threads,
                                       const std::vector<relay_recipient::Member>& roster)
{
  std::vector<std::string> names;
  std::vector<int64_t> seen;
  for (const auto& thread : threads) {
    if (std::ranges::find(seen, thread.fromUserId) != seen.end())
      continue;
    seen.push_back(thread.fromUserId);
    names.push_back(nameOf(roster, thread.fromUserId));
  }
  return names;
}

RelayResult askRecipient(std::vector<std::string> candidates)
{
  return {.step = RelayStep::AskRecipient,
          .outcome = RelayOutcome::NoRecipient,
          .channel = RelayChannel::Notification,
          .candidates = std::move(candidates),
          .threadId = 0,
          .recipientId = 0,
          .recipientName = {}};
}

RelayResult noRecipient()
{
  return {.step = RelayStep::NoRecipient,
          .outcome = RelayOutcome::NoRecipient,
          .channel = RelayChannel::Notification,
          .candidates = {},
          .threadId = 0,
          .recipientId = 0,
          .recipientName = {}};
}

std::vector<std::string> namesOf(const std::vector<relay_recipient::Member>& roster, const std::vector<int64_t>& ids)
{
  std::vector<std::string> names;
  names.reserve(ids.size());
  for (const int64_t id : ids)
    names.push_back(nameOf(roster, id));
  return names;
}

}

RelayService::RelayService(RelayServiceDependencies dependencies)
    : conversation_(std::move(dependencies.conversation)),
      courier_(std::move(dependencies.courier)),
      store_(std::move(dependencies.store)),
      settings_(dependencies.settings)
{
}

void RelayService::withDb(const std::function<void(sqlite3*)>& body) const
{
  if (store_)
    store_->run(body);
}

void RelayService::admit(const std::string& message) const
{
  if (!settings_.enabled)
    throw ResponseException(MessengerErrors::ChannelUnavailable);
  if (blank(message))
    throw ResponseException(MessengerErrors::MessageRequired);
  if (characterCount(message) > settings_.maxMessageChars)
    throw ResponseException(MessengerErrors::MessageTooLong);
}

int64_t RelayService::expiresAt(int64_t now) const
{
  return settings_.retentionDays > 0 ? now + settings_.retentionDays * kDaySeconds : 0;
}

std::string RelayService::commandId(const std::string& prefix, int64_t now, int64_t recipientId) const
{
  return prefix + "-" + std::to_string(now) + "-" + std::to_string(sequence_.fetch_add(1) + 1) + "-" +
         std::to_string(recipientId);
}

RelayResult RelayService::relay(const RelayRequest& request) const
{
  admit(request.message);
  const auto resolution = relay_recipient::resolve(
      {.roster = request.roster, .asked = request.recipient, .senderId = request.senderId});
  switch (resolution.kind) {
  case relay_recipient::ResolutionKind::Self:
    throw ResponseException(MessengerErrors::SelfRecipient);
  case relay_recipient::ResolutionKind::Resolved:
    return deliverTo({.recipientId = resolution.userId,
                      .recipientName = nameOf(request.roster, resolution.userId),
                      .senderId = request.senderId,
                      .senderName = request.senderName,
                      .message = request.message,
                      .lang = request.lang,
                      .replyToKey = {},
                      .dryRun = request.dryRun,
                      .now = request.now});
  case relay_recipient::ResolutionKind::Ambiguous:
  case relay_recipient::ResolutionKind::Missing:
    break;
  }
  if (request.afterQuestion)
    return noRecipient();
  return askRecipient(namesOf(request.roster, resolution.candidates));
}

RelayResult RelayService::reply(const ReplyRequest& request) const
{
  admit(request.message);
  std::vector<RelayThreadRow> threads;
  withDb([&](sqlite3* db) { threads = threadRepository_.openThreads(db, {.toUserId = request.replierId, .now = request.now}); });
  if (request.recipient.empty()) {
    if (threads.empty())
      return askRecipient({});
    const int64_t sender = threads.front().fromUserId;
    if (std::ranges::all_of(threads, [sender](const RelayThreadRow& thread) { return thread.fromUserId == sender; }))
      return answerThreads(threads, request);
    return askRecipient(senderNamesOf(threads, request.roster));
  }
  if (threads.empty())
    return relay({.senderId = request.replierId,
                  .senderName = request.replierName,
                  .recipient = request.recipient,
                  .message = request.message,
                  .lang = request.lang,
                  .afterQuestion = request.afterQuestion,
                  .dryRun = request.dryRun,
                  .roster = request.roster,
                  .now = request.now});
  const auto resolution = relay_recipient::resolve(
      {.roster = sendersOf(threads, request.roster), .asked = request.recipient, .senderId = request.replierId});
  if (resolution.kind == relay_recipient::ResolutionKind::Resolved) {
    std::vector<RelayThreadRow> fromSender;
    std::ranges::copy_if(threads, std::back_inserter(fromSender),
                         [&resolution](const RelayThreadRow& thread) { return thread.fromUserId == resolution.userId; });
    return answerThreads(fromSender, request);
  }
  if (request.afterQuestion)
    return noRecipient();
  return askRecipient(senderNamesOf(threads, request.roster));
}

RelayResult RelayService::answerThreads(const std::vector<RelayThreadRow>& threads, const ReplyRequest& request) const
{
  const int64_t senderId = threads.front().fromUserId;
  RelayResult result = deliverTo({.recipientId = senderId,
                                  .recipientName = nameOf(request.roster, senderId),
                                  .senderId = request.replierId,
                                  .senderName = request.replierName,
                                  .message = request.message,
                                  .lang = request.lang,
                                  .replyToKey = threads.back().messageId,
                                  .dryRun = request.dryRun,
                                  .now = request.now});
  if (request.dryRun)
    return result;
  withDb([&](sqlite3* db) {
    for (const auto& thread : threads)
      threadRepository_.answer(db, thread.id);
  });
  if (courier_)
    for (const auto& thread : threads)
      if (thread.messageId != threads.back().messageId &&
          !courier_->settle({.messageKey = thread.messageId, .settlement = messenger::CourierSettlement::Answered}))
        LOG_WARN << "argus-llm: an answered message could not be marked in the inbox";
  return result;
}

RelayResult RelayService::deliverTo(const DeliveryInput& input) const
{
  const bool reply = !input.replyToKey.empty();
  const std::string command = commandId(reply ? "reply-" + std::to_string(input.senderId)
                                              : "relay-" + std::to_string(input.senderId),
                                        input.now, input.recipientId);
  bool limited = false;
  int64_t threadId = 0;
  withDb([&](sqlite3* db) {
    const int64_t sent = threadRepository_.countFrom(db, {.fromUserId = input.senderId, .since = input.now - kHourSeconds});
    if (sent >= settings_.maxRelaysPerHour) {
      limited = true;
      return;
    }
    if (input.dryRun)
      return;
    threadId = threadRepository_.create(db, {.fromUserId = input.senderId,
                                             .toUserId = input.recipientId,
                                             .messageId = command,
                                             .channel = RelayChannel::Notification,
                                             .replyUntil = input.now + settings_.replyWindowSeconds,
                                             .at = input.now});
  });
  if (limited)
    throw ResponseException(MessengerErrors::RateLimited);
  if (input.dryRun)
    return {.step = RelayStep::Ready,
            .outcome = RelayOutcome::NoRecipient,
            .channel = RelayChannel::Notification,
            .candidates = {},
            .threadId = 0,
            .recipientId = input.recipientId,
            .recipientName = input.recipientName};
  if (threadId <= 0)
    throw ResponseException(MessengerErrors::ChannelUnavailable);
  const std::string spoken = relay_lines::messageFromSender(
      {.sender = input.senderName, .message = input.message, .lang = input.lang});
  const messenger::CourierReceipt receipt =
      courier_ ? courier_->carry({.fromUserId = input.senderId,
                                  .fromName = input.senderName,
                                  .toUserId = input.recipientId,
                                  .toName = input.recipientName,
                                  .kind = reply ? messenger::CourierKind::Reply : messenger::CourierKind::Relay,
                                  .body = input.message,
                                  .spokenLine = spoken,
                                  .title = input.senderName,
                                  .replyToKey = input.replyToKey,
                                  .commandId = command,
                                  .expiresAt = expiresAt(input.now)})
               : messenger::CourierReceipt{};
  if (!receipt.carried) {
    withDb([&](sqlite3* db) { threadRepository_.remove(db, threadId); });
    throw ResponseException(MessengerErrors::ChannelUnavailable);
  }
  withDb([&](sqlite3* db) {
    threadRepository_.setDelivery(db, {.id = threadId,
                                       .channel = receipt.channel,
                                       .messageId = receipt.messageKey.empty() ? command : receipt.messageKey});
  });
  const bool spokenThere = receipt.channel == RelayChannel::Conversation;
  if (spokenThere && conversation_ &&
      !conversation_->note({.userId = input.recipientId, .text = spoken, .at = input.now}))
    LOG_WARN << "argus-llm: a delivered message did not reach the recipient's conversation context";
  return {.step = RelayStep::Sent,
          .outcome = spokenThere ? RelayOutcome::Delivered : RelayOutcome::Notified,
          .channel = receipt.channel,
          .candidates = {},
          .threadId = threadId,
          .recipientId = input.recipientId,
          .recipientName = input.recipientName};
}

VisitorReply RelayService::visitorAsk(const VisitorRequest& request) const
{
  if (request.need.empty())
    return {.needsAnswer = true, .line = relay_lines::visitorAsk(request.lang)};
  const auto resolution = relay_recipient::resolve({.roster = request.roster, .asked = request.recipient, .senderId = 0});
  if (resolution.kind == relay_recipient::ResolutionKind::Resolved) {
    try {
      notifyVisitor({.recipientId = resolution.userId,
                     .recipientName = nameOf(request.roster, resolution.userId),
                     .camera = request.camera,
                     .need = request.need,
                     .lang = request.lang,
                     .now = request.now});
    }
    catch (const ResponseException& refusal) {
      LOG_WARN << "argus-llm: a visitor's notice was not made: " << refusal.what();
    }
  }
  return {.needsAnswer = false, .line = relay_lines::visitorDone(request.lang)};
}

SweepResult RelayService::sweep(int64_t now) const
{
  SweepResult result;
  const int64_t before = now - kRetentionSeconds;
  std::vector<std::string> expired;
  withDb([&](sqlite3* db) {
    expired = threadRepository_.expire(db, now);
    result.threadsPurged = threadRepository_.purge(db, before);
    result.noticesPurged = visitorNotices_.purge(db, before);
  });
  result.threadsExpired = static_cast<int>(expired.size());
  if (!courier_)
    return result;
  for (const auto& messageKey : expired)
    if (courier_->settle({.messageKey = messageKey, .settlement = messenger::CourierSettlement::Expired}))
      ++result.settled;
  return result;
}

int64_t RelayService::reserveVisitorNotice(const VisitorNoticeKey& key) const
{
  int64_t reserved = 0;
  bool limited = false;
  withDb([&](sqlite3* db) {
    const int64_t made = visitorNotices_.countSince(db, {.camera = key.camera, .toUserId = key.recipientId, .since = key.now - kHourSeconds});
    if (made >= settings_.maxVisitorNoticesPerHourPerCamera) {
      limited = true;
      return;
    }
    reserved = visitorNotices_.record(db, {.camera = key.camera, .toUserId = key.recipientId, .at = key.now});
  });
  if (limited)
    throw ResponseException(MessengerErrors::VisitorLimited);
  return reserved;
}

void RelayService::notifyVisitor(const VisitorDelivery& delivery) const
{
  if (!settings_.enabled)
    return;
  const int64_t reserved = reserveVisitorNotice({.camera = delivery.camera, .recipientId = delivery.recipientId, .now = delivery.now});
  const std::string line = relay_lines::visitorMessage({.camera = delivery.camera, .need = delivery.need, .lang = delivery.lang});
  const messenger::CourierReceipt receipt =
      courier_ ? courier_->carry({.fromUserId = 0,
                                  .fromName = {},
                                  .toUserId = delivery.recipientId,
                                  .toName = delivery.recipientName,
                                  .kind = messenger::CourierKind::Visitor,
                                  .body = delivery.need,
                                  .spokenLine = line,
                                  .title = relay_lines::visitorTitle(delivery.camera, delivery.lang),
                                  .replyToKey = {},
                                  .commandId = commandId("visitor", delivery.now, delivery.recipientId),
                                  .expiresAt = expiresAt(delivery.now)})
               : messenger::CourierReceipt{};
  if (!receipt.carried) {
    withDb([&](sqlite3* db) { visitorNotices_.remove(db, reserved); });
    LOG_WARN << "argus-llm: a visitor's message was not carried to a household member";
    return;
  }
  if (receipt.channel == RelayChannel::Conversation && conversation_ &&
      !conversation_->note({.userId = delivery.recipientId, .text = line, .at = delivery.now}))
    LOG_WARN << "argus-llm: a visitor's message did not reach the member's conversation context";
}
