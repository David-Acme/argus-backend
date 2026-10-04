#pragma once

#include <feature/call/vocabulary/call-state.hxx>
#include <feature/call/vocabulary/call-trigger.hxx>

#include <cstdint>
#include <string>
#include <string_view>

namespace call_query
{
inline constexpr std::string_view INSERT =
    "INSERT OR IGNORE INTO call (user_id, dedupe_key, trigger, state, reason, "
    "parent_call_id, urgency, lang, title, summary, opening_line, missed_line, "
    "data, created_at, expires_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id";

inline constexpr std::string_view INSERT_RINGING =
    "INSERT OR IGNORE INTO call (user_id, dedupe_key, trigger, state, reason, "
    "parent_call_id, urgency, lang, title, summary, opening_line, missed_line, "
    "data, created_at, expires_at) "
    "SELECT ?, ?, ?, 'ringing', ?, 0, ?, ?, ?, ?, ?, ?, ?, ?, ? "
    "WHERE NOT EXISTS (SELECT 1 FROM call WHERE user_id = ? "
    "AND state = 'ringing' AND expires_at > ?) RETURNING id";

inline constexpr std::string_view EXISTS =
    "SELECT 1 FROM call WHERE dedupe_key = ? AND user_id = ? LIMIT 1";

inline constexpr std::string_view FIND_BY_ID = "SELECT * FROM call WHERE id = ?";

inline constexpr std::string_view FIND_RINGING_FOR =
    "SELECT * FROM call WHERE user_id = ? AND state = 'ringing' "
    "AND expires_at > ? ORDER BY id DESC LIMIT 1";

inline constexpr std::string_view RING_STATS =
    "SELECT COALESCE(MAX(created_at), 0) AS last_at, "
    "COALESCE(SUM(created_at >= ?), 0) AS recent "
    "FROM call WHERE user_id = ? AND parent_call_id = 0 "
    "AND state IN ('ringing', 'answered', 'completed', 'missed', 'declined')";

inline constexpr std::string_view CLAIM =
    "UPDATE call SET state = 'answered', answered_at = ?, answered_session = ? "
    "WHERE id = ? AND user_id = ? AND state = 'ringing' AND expires_at > ?";

inline constexpr std::string_view MARK_MISSED =
    "UPDATE call SET state = 'missed', ended_at = ? "
    "WHERE id = ? AND state = 'ringing'";

inline constexpr std::string_view END =
    "UPDATE call SET state = ?, ended_at = ? "
    "WHERE id = ? AND user_id = ? AND state = 'answered'";

inline constexpr std::string_view MARK_PUSHED =
    "UPDATE call SET pushed_at = ? WHERE id = ? AND state = 'ringing'";

inline constexpr std::string_view FIND_FOLLOWUPS =
    "SELECT * FROM call WHERE parent_call_id = ? AND state = 'queued' "
    "ORDER BY id ASC";

inline constexpr std::string_view SETTLE_FOLLOWUPS =
    "UPDATE call SET state = ?, ended_at = ? "
    "WHERE parent_call_id = ? AND state = 'queued'";

inline constexpr std::string_view FIND_RINGING =
    "SELECT * FROM call WHERE state = 'ringing' ORDER BY id ASC";

inline constexpr std::string_view CLOSE_STALE_ANSWERED =
    "UPDATE call SET state = 'completed', ended_at = ? "
    "WHERE state = 'answered' AND answered_at < ?";
}

struct CallCreateInput
{
  int64_t userId{0};
  std::string dedupeKey;
  CallTrigger trigger{CallTrigger::Assistant};
  CallState state{CallState::Ringing};
  std::string reason;
  int64_t parentCallId{0};
  std::string urgency;
  std::string lang;
  std::string title;
  std::string summary;
  std::string openingLine;
  std::string missedLine;
  std::string data;
  int64_t createdAt{0};
  int64_t expiresAt{0};
};

struct CallRingStats
{
  int64_t lastAt{0};
  int recent{0};
};

struct CallRingStatsInput
{
  int64_t userId{0};
  int64_t since{0};
};

struct CallRingingInput
{
  int64_t userId{0};
  int64_t now{0};
};

struct CallClaimInput
{
  int64_t id{0};
  int64_t userId{0};
  std::string sessionId;
  int64_t now{0};
};

struct CallEndInput
{
  int64_t id{0};
  int64_t userId{0};
  CallState state{CallState::Completed};
  int64_t now{0};
};

struct CallSettleInput
{
  int64_t parentId{0};
  CallState state{CallState::Missed};
  int64_t now{0};
};
