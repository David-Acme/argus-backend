#include "conversation-message-repository.hxx"

#include <sqlite/db-service.hxx>
#include <sync/sync-limits.hxx>

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

using namespace conversation_message_query;

namespace
{
constexpr int64_t kNoBound = std::numeric_limits<int64_t>::max();

std::vector<ConversationMessageSchema> schemasOf(const drogon::orm::Result& rows)
{
  std::vector<ConversationMessageSchema> out;
  out.reserve(rows.size());
  for (const auto& row : rows)
    out.emplace_back(row);
  return out;
}

std::optional<std::string> legName(const std::optional<ConversationLegKind>& leg)
{
  if (!leg)
    return std::nullopt;
  return conversationLegKindToString(*leg);
}

std::optional<int64_t> readAtOf(const ConversationCopyInput& copy)
{
  if (!copy.isRead)
    return std::nullopt;
  return copy.at;
}

struct SyncWindow
{
  int64_t startTime{-1};
  int64_t startId{-1};
  int64_t endTime{kNoBound};
};

SyncWindow windowOf(const ConversationSyncFilter& filter)
{
  return {.startTime = filter.startTime.value_or(-1),
          .startId = filter.startId.value_or(-1),
          .endTime = filter.endTime.value_or(kNoBound)};
}
}

drogon::Task<ConversationCopiesResult>
ConversationMessageRepository::insertCopies(const ConversationCopiesInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  ConversationCopiesResult result;
  for (const auto& copy : input.copies) {
    const auto rows = co_await client->execSqlCoro(std::string(INSERT_COPY),
                                                   copy.userId,
                                                   copy.counterpartId,
                                                   copy.counterpartName,
                                                   messageDirectionToString(copy.direction),
                                                   messageKindToString(copy.kind),
                                                   copy.body,
                                                   legName(copy.leg),
                                                   messageStateToString(copy.state),
                                                   copy.messageKey,
                                                   copy.replyToKey,
                                                   copy.isRead ? 1 : 0,
                                                   readAtOf(copy),
                                                   copy.expiresAt,
                                                   copy.at);
    for (const auto& row : rows)
      result.inserted.emplace_back(row);
  }
  result.duplicate = !input.copies.empty() && result.inserted.empty();
  co_return result;
}

drogon::Task<std::optional<ConversationMessageSchema>>
ConversationMessageRepository::findCopy(const ConversationCopyRef& ref) const
{
  const auto pooled = DbService::client();
  auto* client = ref.client != nullptr ? ref.client : pooled.get();
  const auto rows = co_await client->execSqlCoro(std::string(FIND_COPY), ref.messageKey, ref.userId);
  if (rows.empty())
    co_return std::nullopt;
  co_return ConversationMessageSchema(rows.front());
}

drogon::Task<std::vector<ConversationMessageChange>>
ConversationMessageRepository::transition(const ConversationTransitionInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  const auto rows = co_await client->execSqlCoro(std::string(FIND_OUTGOING), input.messageKey);
  std::vector<ConversationMessageChange> changes;
  for (const auto& row : rows) {
    ConversationMessageSchema before(row);
    if (before.state == input.state || std::ranges::find(input.from, before.state) == input.from.end())
      continue;
    const auto updated = co_await client->execSqlCoro(
        std::string(SET_STATE), messageStateToString(input.state), input.at, before.id);
    if (updated.empty())
      continue;
    changes.push_back({.before = std::move(before), .after = ConversationMessageSchema(updated.front())});
  }
  co_return changes;
}

drogon::Task<std::vector<ConversationMessageChange>>
ConversationMessageRepository::markThreadRead(const ConversationThreadInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  const auto rows =
      co_await client->execSqlCoro(std::string(FIND_UNREAD_THREAD), input.userId, input.counterpartId);
  if (rows.empty())
    co_return {};
  std::vector<ConversationMessageChange> changes;
  changes.reserve(rows.size());
  for (const auto& row : rows) {
    ConversationMessageSchema before(row);
    ConversationMessageSchema after = before;
    after.isRead = true;
    after.readAt = input.at;
    after.updatedAt = input.at;
    changes.push_back({.before = std::move(before), .after = std::move(after)});
  }
  co_await client->execSqlCoro(
      std::string(MARK_THREAD_READ), input.at, input.at, input.userId, input.counterpartId);
  co_return changes;
}

drogon::Task<std::vector<ConversationMessageChange>>
ConversationMessageRepository::removeThread(const ConversationThreadInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  const auto existing = co_await client->execSqlCoro(std::string(FIND_THREAD), input.userId, input.counterpartId);
  if (existing.empty())
    co_return {};
  std::unordered_map<int64_t, ConversationMessageSchema> beforeById;
  beforeById.reserve(existing.size());
  for (const auto& row : existing) {
    ConversationMessageSchema before(row);
    const int64_t id = before.id;
    beforeById.emplace(id, std::move(before));
  }
  const auto removed = co_await client->execSqlCoro(
      std::string(SOFT_DELETE_THREAD), input.at, input.at, input.userId, input.counterpartId);
  std::vector<ConversationMessageChange> changes;
  changes.reserve(removed.size());
  for (const auto& row : removed) {
    ConversationMessageSchema after(row);
    const auto before = beforeById.find(after.id);
    if (before == beforeById.end())
      continue;
    changes.push_back({.before = std::move(before->second), .after = std::move(after)});
  }
  co_return changes;
}

drogon::Task<std::vector<ConversationThreadSummary>> ConversationMessageRepository::threads(int64_t userId) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(FIND_THREADS), userId);
  std::vector<ConversationThreadSummary> out;
  out.reserve(rows.size());
  for (const auto& row : rows)
    out.push_back({.last = ConversationMessageSchema(row),
                   .unread = static_cast<int64_t>(row["unread"].as<long long>())});
  co_return out;
}

drogon::Task<std::vector<ConversationMessageSchema>>
ConversationMessageRepository::findSync(const ConversationSyncFilter& filter) const
{
  const SyncWindow window = windowOf(filter);
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(FIND_SYNC) + SyncLimits::kMaxRows,
                                                              filter.userId,
                                                              window.startTime,
                                                              window.startTime,
                                                              window.startId,
                                                              window.endTime);
  co_return schemasOf(rows);
}

drogon::Task<std::optional<ConversationMessageSchema>> ConversationMessageRepository::findLastSync(int64_t userId) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(FIND_LAST_SYNC), userId);
  if (rows.empty())
    co_return std::nullopt;
  co_return ConversationMessageSchema(rows.front());
}

drogon::Task<std::vector<ConversationMessageSchema>>
ConversationMessageRepository::findDeletedSync(const ConversationSyncFilter& filter) const
{
  const SyncWindow window = windowOf(filter);
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(FIND_DELETED_SYNC) + SyncLimits::kMaxRows,
                                                filter.userId,
                                                window.startTime,
                                                window.startTime,
                                                window.startId,
                                                window.endTime);
  co_return schemasOf(rows);
}

drogon::Task<std::optional<ConversationMessageSchema>>
ConversationMessageRepository::findLastDeletedSync(int64_t userId) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(FIND_LAST_DELETED_SYNC), userId);
  if (rows.empty())
    co_return std::nullopt;
  co_return ConversationMessageSchema(rows.front());
}

drogon::Task<std::vector<ConversationMessageSchema>>
ConversationMessageRepository::expire(const ConversationExpireInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  const auto rows = co_await client->execSqlCoro(std::string(EXPIRE_BATCH), input.now, input.now, input.now);
  co_return schemasOf(rows);
}

drogon::Task<void> ConversationMessageRepository::purgeDeleted(int64_t before) const
{
  co_await DbService::client()->execSqlCoro(std::string(PURGE_DELETED), before);
}
