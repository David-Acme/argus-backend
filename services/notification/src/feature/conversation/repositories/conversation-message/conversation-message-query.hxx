#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <feature/conversation/schemas/conversation-message/conversation-message-schema.hxx>
#include <llm/conversation-leg.hxx>
#include <notification/message-direction.hxx>
#include <notification/message-kind.hxx>
#include <notification/message-state.hxx>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace conversation_message_query
{
inline constexpr std::string_view INSERT_COPY =
    "INSERT OR IGNORE INTO conversation_message (user_id, counterpart_id, counterpart_name, direction, kind, body, "
    "leg, state, message_key, reply_to_key, is_read, read_at, expires_at, created_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING *";

inline constexpr std::string_view FIND_COPY =
    "SELECT * FROM conversation_message WHERE message_key = ? AND user_id = ?";

inline constexpr std::string_view FIND_OUTGOING =
    "SELECT * FROM conversation_message WHERE message_key = ? AND direction = 'out' AND deleted_at IS NULL";

inline constexpr std::string_view SET_STATE =
    "UPDATE conversation_message SET state = ?, updated_at = ? "
    "WHERE id = ? AND deleted_at IS NULL RETURNING *";

inline constexpr std::string_view FIND_UNREAD_THREAD =
    "SELECT * FROM conversation_message WHERE user_id = ? AND counterpart_id = ? AND is_read = 0 "
    "AND deleted_at IS NULL ORDER BY id ASC";

inline constexpr std::string_view MARK_THREAD_READ =
    "UPDATE conversation_message SET is_read = 1, read_at = ?, updated_at = ? "
    "WHERE user_id = ? AND counterpart_id = ? AND is_read = 0 AND deleted_at IS NULL";

inline constexpr std::string_view FIND_THREAD =
    "SELECT * FROM conversation_message WHERE user_id = ? AND counterpart_id = ? AND deleted_at IS NULL "
    "ORDER BY id ASC";

inline constexpr std::string_view SOFT_DELETE_THREAD =
    "UPDATE conversation_message SET deleted_at = ?, updated_at = ? "
    "WHERE user_id = ? AND counterpart_id = ? AND deleted_at IS NULL RETURNING *";

inline constexpr std::string_view FIND_THREADS =
    "SELECT m.*, (SELECT COUNT(*) FROM conversation_message u WHERE u.user_id = m.user_id "
    "AND u.counterpart_id = m.counterpart_id AND u.is_read = 0 AND u.deleted_at IS NULL) AS unread "
    "FROM conversation_message m WHERE m.user_id = ? AND m.deleted_at IS NULL AND m.id = ("
    "SELECT MAX(l.id) FROM conversation_message l WHERE l.user_id = m.user_id "
    "AND l.counterpart_id = m.counterpart_id AND l.deleted_at IS NULL) "
    "ORDER BY m.created_at DESC, m.id DESC LIMIT 100";

inline constexpr std::string_view FIND_SYNC =
    "SELECT * FROM conversation_message WHERE user_id = ? AND deleted_at IS NULL "
    "AND (created_at > ? OR (created_at = ? AND id > ?)) AND created_at <= ? "
    "ORDER BY created_at ASC, id ASC LIMIT ";

inline constexpr std::string_view FIND_LAST_SYNC =
    "SELECT * FROM conversation_message WHERE user_id = ? AND deleted_at IS NULL "
    "ORDER BY created_at DESC, id DESC LIMIT 1";

inline constexpr std::string_view FIND_DELETED_SYNC =
    "SELECT * FROM conversation_message WHERE user_id = ? AND deleted_at IS NOT NULL "
    "AND (deleted_at > ? OR (deleted_at = ? AND id > ?)) AND deleted_at <= ? "
    "ORDER BY deleted_at ASC, id ASC LIMIT ";

inline constexpr std::string_view FIND_LAST_DELETED_SYNC =
    "SELECT * FROM conversation_message WHERE user_id = ? AND deleted_at IS NOT NULL "
    "ORDER BY deleted_at DESC, id DESC LIMIT 1";

inline constexpr std::string_view EXPIRE_BATCH =
    "UPDATE conversation_message SET deleted_at = ?, updated_at = ? WHERE id IN ("
    "SELECT id FROM conversation_message WHERE deleted_at IS NULL AND expires_at > 0 AND expires_at < ? "
    "ORDER BY id ASC LIMIT 200) RETURNING *";

inline constexpr std::string_view PURGE_DELETED =
    "DELETE FROM conversation_message WHERE deleted_at IS NOT NULL AND deleted_at < ?";
}

struct ConversationCopyInput
{
  int64_t userId{0};
  int64_t counterpartId{0};
  std::string counterpartName;
  MessageDirection direction{MessageDirection::In};
  MessageKind kind{MessageKind::Turn};
  std::string body;
  std::optional<ConversationLegKind> leg;
  MessageState state{MessageState::Received};
  std::string messageKey;
  std::optional<std::string> replyToKey;
  bool isRead{false};
  int64_t expiresAt{0};
  int64_t at{0};
};

struct ConversationCopiesInput
{
  std::vector<ConversationCopyInput> copies;
  drogon::orm::DbClient* client{nullptr};
};

struct ConversationCopiesResult
{
  std::vector<ConversationMessageSchema> inserted;
  bool duplicate{false};
};

struct ConversationCopyRef
{
  std::string messageKey;
  int64_t userId{0};
  drogon::orm::DbClient* client{nullptr};
};

struct ConversationTransitionInput
{
  std::string messageKey;
  MessageState state{MessageState::Sent};
  std::vector<MessageState> from;
  int64_t at{0};
  drogon::orm::DbClient* client{nullptr};
};

struct ConversationThreadInput
{
  int64_t userId{0};
  int64_t counterpartId{0};
  int64_t at{0};
  drogon::orm::DbClient* client{nullptr};
};

struct ConversationSyncFilter
{
  int64_t userId{0};
  std::optional<int64_t> startTime;
  std::optional<int64_t> startId;
  std::optional<int64_t> endTime;
};

struct ConversationExpireInput
{
  int64_t now{0};
  drogon::orm::DbClient* client{nullptr};
};

struct ConversationMessageChange
{
  ConversationMessageSchema before;
  ConversationMessageSchema after;
};

struct ConversationThreadSummary
{
  ConversationMessageSchema last;
  int64_t unread{0};
};
