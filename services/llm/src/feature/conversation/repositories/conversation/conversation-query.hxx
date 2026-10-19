#pragma once

#include <llm/conversation-leg.hxx>
#include <feature/conversation/vocabulary/conversation-turn-role.hxx>

#include <cstdint>
#include <string>

namespace conversation_query
{
inline constexpr const char* UPSERT_LEG =
    "INSERT INTO conversation (user_id, active_leg, last_turn_at, created_at, updated_at) "
    "VALUES (?1, ?2, 0, ?3, ?3) "
    "ON CONFLICT (user_id) DO UPDATE SET active_leg = excluded.active_leg, updated_at = excluded.updated_at";
inline constexpr const char* SELECT_CONVERSATION =
    "SELECT user_id, active_leg, summary, summary_turns, last_turn_at, created_at, updated_at "
    "FROM conversation WHERE user_id = ?";
inline constexpr const char* INSERT_TURN =
    "INSERT INTO conversation_turn (user_id, role, leg, content, created_at) VALUES (?, ?, ?, ?, ?)";
inline constexpr const char* TOUCH_LAST_TURN =
    "UPDATE conversation SET last_turn_at = ?, updated_at = ? WHERE user_id = ?";
inline constexpr const char* SELECT_RECENT_TURNS =
    "SELECT id, user_id, role, leg, content, created_at FROM ("
    "SELECT id, user_id, role, leg, content, created_at FROM conversation_turn "
    "WHERE user_id = ? ORDER BY id DESC LIMIT ?) ORDER BY id ASC";
inline constexpr const char* UPDATE_SUMMARY =
    "UPDATE conversation SET summary = ?, summary_turns = ?, updated_at = ? WHERE user_id = ?";
inline constexpr const char* PURGE_TURNS_BEFORE = "DELETE FROM conversation_turn WHERE created_at < ?";
inline constexpr const char* SELECT_IDLE_USERS =
    "SELECT user_id FROM conversation WHERE last_turn_at > 0 AND last_turn_at < ? ORDER BY user_id";
}

struct ConversationRow
{
  int64_t userId{0};
  ConversationLegKind activeLeg{ConversationLegKind::Phone};
  std::string summary;
  int64_t summaryTurns{0};
  int64_t lastTurnAt{0};
  int64_t createdAt{0};
  int64_t updatedAt{0};
};

struct ConversationTurnRow
{
  int64_t id{0};
  int64_t userId{0};
  ConversationTurnRole role{ConversationTurnRole::User};
  ConversationLegKind leg{ConversationLegKind::Phone};
  std::string content;
  int64_t createdAt{0};
};

struct ConversationLegTouchInput
{
  int64_t userId{0};
  ConversationLegKind leg{ConversationLegKind::Phone};
  int64_t at{0};
};

struct ConversationTurnAppendInput
{
  int64_t userId{0};
  ConversationTurnRole role{ConversationTurnRole::User};
  ConversationLegKind leg{ConversationLegKind::Phone};
  std::string content;
  int64_t at{0};
};

struct ConversationRecentTurnsInput
{
  int64_t userId{0};
  int64_t limit{0};
};

struct ConversationSummaryInput
{
  int64_t userId{0};
  std::string summary;
  int64_t summaryTurns{0};
  int64_t at{0};
};
