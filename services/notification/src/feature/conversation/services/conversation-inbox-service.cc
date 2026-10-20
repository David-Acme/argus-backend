#include "conversation-inbox-service.hxx"

#include <errors/response-exception.hxx>
#include <notification/notification-errors.hxx>
#include <runtime/blocking-task.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <sync/user-change-sink.hxx>
#include <trantor/utils/Logger.h>

#include <ctime>
#include <exception>
#include <string>
#include <utility>

namespace
{
constexpr int64_t kDeletedRetentionS = 7LL * 24 * 3600;
constexpr const char* kHouseholdMessageType = "household_message";

int64_t nowSeconds()
{
  return static_cast<int64_t>(std::time(nullptr));
}

bool blank(const std::string& text)
{
  return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

SocketEmitDto addOf(const ConversationMessageSchema& row)
{
  SocketEmitDto frame;
  frame.operation = SyncOperation::Add;
  frame.option = TableName::ConversationMessage;
  frame.obj = row.toJson();
  return frame;
}

SocketEmitDto tombstoneOf(const ConversationMessageSchema& row)
{
  SocketEmitDto frame;
  frame.operation = SyncOperation::Delete;
  frame.option = TableName::ConversationMessage;
  frame.obj["id"] = static_cast<Json::Int64>(row.id);
  frame.obj["deletedAt"] = static_cast<Json::Int64>(row.deletedAt.value_or(0));
  return frame;
}

struct EmitRowsInput
{
  std::vector<ConversationMessageSchema> rows;
  bool tombstones{false};
  drogon::orm::DbClient* client{nullptr};
};

drogon::Task<void> emitRows(const EmitRowsInput& input)
{
  const UserEmitSink* sink = user_change::getNotificationSink();
  if (sink == nullptr) {
    if (!input.rows.empty())
      LOG_WARN << "user change sink not installed; drop conversation emits";
    co_return;
  }
  for (const auto& row : input.rows)
    co_await sink->emitUsers({.userIds = {row.userId},
                              .body = input.tombstones ? tombstoneOf(row) : addOf(row),
                              .client = input.client});
}

struct AuditChangesInput
{
  std::vector<ConversationMessageChange> changes;
  drogon::orm::DbClient* client{nullptr};
};

drogon::Task<void> publishChanges(const AuditChangesInput& input)
{
  const UserEmitSink* sink = user_change::getNotificationSink();
  if (sink == nullptr) {
    if (!input.changes.empty())
      LOG_WARN << "user change sink not installed; drop conversation audits";
    co_return;
  }
  for (const auto& change : input.changes)
    co_await sink->publishAudit({.recordId = change.after.id,
                                 .tableName = TableName::ConversationMessage,
                                 .before = change.before.toJson(),
                                 .after = change.after.toJson(),
                                 .userIds = {change.after.userId},
                                 .client = input.client});
}

std::vector<ConversationCopyInput> copiesOf(const RelayLetter& letter, int64_t at)
{
  std::vector<ConversationCopyInput> copies;
  if (letter.fromUserId > 0)
    copies.push_back({.userId = letter.fromUserId,
                      .counterpartId = letter.toUserId,
                      .counterpartName = letter.toName,
                      .direction = MessageDirection::Out,
                      .kind = letter.kind,
                      .body = letter.body,
                      .leg = std::nullopt,
                      .state = MessageState::Sent,
                      .messageKey = letter.commandId,
                      .replyToKey = letter.replyToKey,
                      .isRead = true,
                      .expiresAt = letter.expiresAt,
                      .at = at});
  const bool visitor = letter.kind == MessageKind::Visitor;
  copies.push_back({.userId = letter.toUserId,
                    .counterpartId = visitor ? 0 : letter.fromUserId,
                    .counterpartName = visitor ? letter.title : letter.fromName,
                    .direction = MessageDirection::In,
                    .kind = letter.kind,
                    .body = letter.body,
                    .leg = std::nullopt,
                    .state = MessageState::Received,
                    .messageKey = letter.commandId,
                    .replyToKey = letter.replyToKey,
                    .isRead = false,
                    .expiresAt = letter.expiresAt,
                    .at = at});
  return copies;
}

Json::Value noticeData(const RelayLetter& letter)
{
  Json::Value data(Json::objectValue);
  data["kind"] = kHouseholdMessageType;
  data["fromUserId"] = static_cast<Json::Int64>(letter.fromUserId);
  data["messageKey"] = letter.commandId;
  data["counterpartId"] =
      static_cast<Json::Int64>(letter.kind == MessageKind::Visitor ? 0 : letter.fromUserId);
  return data;
}

template <typename Result, typename Body>
drogon::Task<Result> inTransaction(Body body)
{
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    Result result = co_await body(transaction.get());
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(NotificationErrors::ChangeNotRecorded);
    co_return result;
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
}
}

bool validLetter(const RelayLetter& letter)
{
  if (letter.toUserId <= 0 || letter.fromUserId < 0 || letter.fromUserId == letter.toUserId)
    return false;
  if (letter.kind == MessageKind::Turn)
    return false;
  if ((letter.kind == MessageKind::Visitor) != (letter.fromUserId == 0))
    return false;
  if (letter.kind == MessageKind::Reply && (!letter.replyToKey || letter.replyToKey->empty()))
    return false;
  if (blank(letter.body) || letter.body.size() > ConversationInboxService::kMaxBodyBytes)
    return false;
  if (letter.spokenLine.size() > ConversationInboxService::kMaxBodyBytes ||
      letter.title.size() > ConversationInboxService::kMaxNameBytes ||
      letter.fromName.size() > ConversationInboxService::kMaxNameBytes ||
      letter.toName.size() > ConversationInboxService::kMaxNameBytes)
    return false;
  if (letter.replyToKey && letter.replyToKey->size() > ConversationInboxService::kMaxKeyBytes)
    return false;
  return !letter.commandId.empty() && letter.commandId.size() <= ConversationInboxService::kMaxKeyBytes &&
         letter.expiresAt >= 0;
}

bool validTranscript(const TranscriptInput& input)
{
  if (input.userId <= 0 || input.lines.empty() || input.lines.size() > ConversationInboxService::kMaxTranscriptLines)
    return false;
  if (input.commandId.empty() || input.commandId.size() > ConversationInboxService::kMaxKeyBytes ||
      input.expiresAt < 0)
    return false;
  for (const auto& line : input.lines)
    if (blank(line.body) || line.body.size() > ConversationInboxService::kMaxBodyBytes)
      return false;
  return true;
}

ConversationInboxService::ConversationInboxService(Dependencies dependencies)
    : dependencies_(std::move(dependencies))
{
}

drogon::Task<RelayReceipt> ConversationInboxService::relay(const RelayLetter& letter) const
{
  if (!validLetter(letter))
    throw ResponseException(NotificationErrors::MessageInvalid);
  const int64_t at = nowSeconds();
  const auto copies = copiesOf(letter, at);
  const ConversationCopiesResult stored =
      co_await inTransaction<ConversationCopiesResult>([this, copies, letter, at](drogon::orm::DbClient* client)
                                                           -> drogon::Task<ConversationCopiesResult> {
        const ConversationCopiesInput insert{.copies = copies, .client = client};
        ConversationCopiesResult result = co_await repository_.insertCopies(insert);
        if (result.duplicate)
          co_return result;
        const EmitRowsInput added{.rows = result.inserted, .tombstones = false, .client = client};
        co_await emitRows(added);
        if (letter.replyToKey) {
          const ConversationTransitionInput answer{
              .messageKey = *letter.replyToKey,
              .state = MessageState::Answered,
              .from = {MessageState::Sent, MessageState::Delivered, MessageState::Notified, MessageState::Expired},
              .at = at,
              .client = client};
          const AuditChangesInput answered{.changes = co_await repository_.transition(answer), .client = client};
          co_await publishChanges(answered);
        }
        co_return result;
      });
  if (stored.duplicate) {
    const ConversationCopyRef ref{.messageKey = letter.commandId,
                                  .userId = letter.fromUserId > 0 ? letter.fromUserId : letter.toUserId,
                                  .client = nullptr};
    const auto copy = co_await repository_.findCopy(ref);
    const bool spoken = copy && copy->state == MessageState::Delivered;
    co_return RelayReceipt{.messageKey = letter.commandId,
                           .route = spoken ? RelayRoute::Conversation : RelayRoute::Notification,
                           .duplicate = true};
  }
  const RelayRoute route = co_await deliver(letter);
  const SettleLetter moved{.messageKey = letter.commandId,
                           .state = route == RelayRoute::Conversation ? MessageState::Delivered
                                                                       : MessageState::Notified};
  co_await moveOutgoing(moved);
  co_return RelayReceipt{.messageKey = letter.commandId, .route = route, .duplicate = false};
}

drogon::Task<bool> ConversationInboxService::announcesLive(int64_t userId) const
{
  if (!dependencies_.liveAnnounce)
    co_return true;
  try {
    co_return co_await dependencies_.liveAnnounce->allows(userId);
  }
  catch (const std::exception& error) {
    LOG_WARN << "Conversation: the live announce preference could not be read; the message goes as a notice: "
             << error.what();
  }
  co_return false;
}

drogon::Task<RelayRoute> ConversationInboxService::deliver(const RelayLetter& letter) const
{
  if (dependencies_.announcer && !letter.spokenLine.empty() && co_await announcesLive(letter.toUserId)) {
    const auto announcer = dependencies_.announcer;
    const MessageAnnouncement announcement{.userId = letter.toUserId, .text = letter.spokenLine};
    try {
      const std::optional<bool> heard = co_await BlockingTask<std::optional<bool>>(
          [announcer, announcement]() { return announcer->announce(announcement); });
      if (heard.value_or(false))
        co_return RelayRoute::Conversation;
    }
    catch (const std::exception& error) {
      LOG_WARN << "Conversation: the live announcement failed, the message goes as a notice: " << error.what();
    }
  }
  if (dependencies_.notices) {
    try {
      const MessageNotice notice{.userId = letter.toUserId,
                                 .title = letter.title,
                                 .body = letter.body,
                                 .data = noticeData(letter),
                                 .commandId = "conversation:" + letter.commandId};
      const bool written = co_await dependencies_.notices->notify(notice);
      if (!written)
        LOG_WARN << "Conversation: the notice of a message was not written; it stays in the inbox";
    }
    catch (const std::exception& error) {
      LOG_WARN << "Conversation: the notice of a message failed; it stays in the inbox: " << error.what();
    }
  }
  co_return RelayRoute::Notification;
}

drogon::Task<void> ConversationInboxService::moveOutgoing(const SettleLetter& letter) const
{
  const int64_t at = nowSeconds();
  static_cast<void>(co_await inTransaction<bool>(
      [this, letter, at](drogon::orm::DbClient* client) -> drogon::Task<bool> {
        const ConversationTransitionInput move{
            .messageKey = letter.messageKey, .state = letter.state, .from = {MessageState::Sent}, .at = at,
            .client = client};
        const AuditChangesInput moved{.changes = co_await repository_.transition(move), .client = client};
        co_await publishChanges(moved);
        co_return true;
      }));
}

drogon::Task<int64_t> ConversationInboxService::settle(const SettleLetter& letter) const
{
  if (letter.messageKey.empty() || letter.messageKey.size() > kMaxKeyBytes)
    throw ResponseException(NotificationErrors::MessageInvalid);
  if (letter.state != MessageState::Answered && letter.state != MessageState::Expired)
    throw ResponseException(NotificationErrors::MessageInvalid);
  const int64_t at = nowSeconds();
  const std::vector<MessageState> from = letter.state == MessageState::Answered
                                             ? std::vector<MessageState>{MessageState::Sent, MessageState::Delivered,
                                                                         MessageState::Notified, MessageState::Expired}
                                             : std::vector<MessageState>{MessageState::Sent, MessageState::Delivered,
                                                                         MessageState::Notified};
  co_return co_await inTransaction<int64_t>(
      [this, letter, from, at](drogon::orm::DbClient* client) -> drogon::Task<int64_t> {
        const ConversationTransitionInput settle{
            .messageKey = letter.messageKey, .state = letter.state, .from = from, .at = at, .client = client};
        const AuditChangesInput settled{.changes = co_await repository_.transition(settle), .client = client};
        co_await publishChanges(settled);
        co_return static_cast<int64_t>(settled.changes.size());
      });
}

drogon::Task<TranscriptOutcome> ConversationInboxService::appendTranscript(const TranscriptInput& input) const
{
  if (!validTranscript(input))
    throw ResponseException(NotificationErrors::MessageInvalid);
  const int64_t at = nowSeconds();
  std::vector<ConversationCopyInput> copies;
  copies.reserve(input.lines.size());
  for (std::size_t index = 0; index < input.lines.size(); ++index)
    copies.push_back({.userId = input.userId,
                      .counterpartId = 0,
                      .counterpartName = {},
                      .direction = input.lines[index].direction,
                      .kind = MessageKind::Turn,
                      .body = input.lines[index].body,
                      .leg = input.leg,
                      .state = MessageState::Delivered,
                      .messageKey = input.commandId + ":" + std::to_string(index),
                      .replyToKey = std::nullopt,
                      .isRead = true,
                      .expiresAt = input.expiresAt,
                      .at = at});
  co_return co_await inTransaction<TranscriptOutcome>(
      [this, copies](drogon::orm::DbClient* client) -> drogon::Task<TranscriptOutcome> {
        const ConversationCopiesInput insert{.copies = copies, .client = client};
        const ConversationCopiesResult result = co_await repository_.insertCopies(insert);
        const EmitRowsInput added{.rows = result.inserted, .tombstones = false, .client = client};
        co_await emitRows(added);
        co_return TranscriptOutcome{.appended = static_cast<int64_t>(result.inserted.size()),
                                    .duplicate = result.duplicate};
      });
}

drogon::Task<std::vector<ConversationThreadSummary>> ConversationInboxService::threads(int64_t userId) const
{
  co_return co_await repository_.threads(userId);
}

drogon::Task<int64_t> ConversationInboxService::markThreadRead(const ConversationThreadRef& ref) const
{
  const int64_t at = nowSeconds();
  co_return co_await inTransaction<int64_t>([this, ref, at](drogon::orm::DbClient* client) -> drogon::Task<int64_t> {
    const ConversationThreadInput thread{
        .userId = ref.userId, .counterpartId = ref.counterpartId, .at = at, .client = client};
    const AuditChangesInput read{.changes = co_await repository_.markThreadRead(thread), .client = client};
    co_await publishChanges(read);
    co_return static_cast<int64_t>(read.changes.size());
  });
}

drogon::Task<bool> ConversationInboxService::removeThread(const ConversationThreadRef& ref) const
{
  const int64_t at = nowSeconds();
  co_return co_await inTransaction<bool>([this, ref, at](drogon::orm::DbClient* client) -> drogon::Task<bool> {
    const ConversationThreadInput thread{
        .userId = ref.userId, .counterpartId = ref.counterpartId, .at = at, .client = client};
    const AuditChangesInput removed{.changes = co_await repository_.removeThread(thread), .client = client};
    if (removed.changes.empty())
      co_return false;
    EmitRowsInput tombstones{.rows = {}, .tombstones = true, .client = client};
    tombstones.rows.reserve(removed.changes.size());
    for (const auto& change : removed.changes)
      tombstones.rows.push_back(change.after);
    co_await publishChanges(removed);
    co_await emitRows(tombstones);
    co_return true;
  });
}

drogon::Task<int64_t> ConversationInboxService::expire(int64_t now) const
{
  const int64_t expired =
      co_await inTransaction<int64_t>([this, now](drogon::orm::DbClient* client) -> drogon::Task<int64_t> {
        const ConversationExpireInput expiry{.now = now, .client = client};
        const EmitRowsInput tombstones{.rows = co_await repository_.expire(expiry), .tombstones = true, .client = client};
        co_await emitRows(tombstones);
        co_return static_cast<int64_t>(tombstones.rows.size());
      });
  co_await repository_.purgeDeleted(now - kDeletedRetentionS);
  co_return expired;
}
