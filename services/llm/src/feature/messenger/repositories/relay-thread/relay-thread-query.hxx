#pragma once

#include <feature/messenger/vocabulary/relay-outcome.hxx>
#include <feature/messenger/vocabulary/relay-thread-state.hxx>

#include <cstdint>
#include <string>

namespace relay_thread_query
{
inline constexpr const char* INSERT_THREAD =
    "INSERT INTO relay_thread (from_user_id, to_user_id, message_id, channel, state, reply_until, created_at) "
    "VALUES (?, ?, ?, ?, 'open', ?, ?)";
inline constexpr const char* SELECT_COLUMNS =
    "SELECT id, from_user_id, to_user_id, message_id, channel, state, reply_until, created_at FROM relay_thread ";
inline constexpr const char* WHERE_OPEN_FOR_RECIPIENT =
    "WHERE to_user_id = ? AND state = 'open' AND reply_until >= ? ORDER BY id DESC LIMIT 1";
inline constexpr const char* WHERE_OPEN_THREADS_FOR_RECIPIENT =
    "WHERE to_user_id = ? AND state = 'open' AND reply_until >= ? ORDER BY id";
inline constexpr const char* COUNT_FROM_SENDER_SINCE =
    "SELECT COUNT(*) FROM relay_thread WHERE from_user_id = ? AND created_at >= ?";
inline constexpr const char* REMOVE_THREAD = "DELETE FROM relay_thread WHERE id = ?";
inline constexpr const char* SET_DELIVERY = "UPDATE relay_thread SET channel = ?, message_id = ? WHERE id = ?";
inline constexpr const char* ANSWER_THREAD =
    "UPDATE relay_thread SET state = 'answered' WHERE id = ? AND state = 'open'";
inline constexpr const char* EXPIRE_THREADS =
    "UPDATE relay_thread SET state = 'expired' WHERE state = 'open' AND reply_until < ? RETURNING message_id";
inline constexpr const char* PURGE_SETTLED =
    "DELETE FROM relay_thread WHERE state IN ('answered', 'expired') AND reply_until < ?";
}

struct RelayThreadCreateInput
{
  int64_t fromUserId{0};
  int64_t toUserId{0};
  std::string messageId;
  RelayChannel channel{RelayChannel::Notification};
  int64_t replyUntil{0};
  int64_t at{0};
};

struct RelayThreadDeliveryInput
{
  int64_t id{0};
  RelayChannel channel{RelayChannel::Notification};
  std::string messageId;
};

struct RelayThreadOpenInput
{
  int64_t toUserId{0};
  int64_t now{0};
};

struct RelayThreadCountInput
{
  int64_t fromUserId{0};
  int64_t since{0};
};

struct RelayThreadRow
{
  int64_t id{0};
  int64_t fromUserId{0};
  int64_t toUserId{0};
  std::string messageId;
  RelayChannel channel{RelayChannel::Notification};
  RelayThreadState state{RelayThreadState::Open};
  int64_t replyUntil{0};
  int64_t createdAt{0};
};
