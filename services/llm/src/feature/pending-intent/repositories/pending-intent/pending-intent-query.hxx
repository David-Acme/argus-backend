#pragma once

#include <feature/pending-intent/vocabulary/pending-intent-state.hxx>

#include <cstdint>
#include <string>

namespace pending_intent_query
{
inline constexpr const char* INSERT_OFFER =
    "INSERT INTO pending_intent (user_id, role, module, tool, arguments, lang, utterance, session_id, state, "
    "created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'offered', ?, ?)";
inline constexpr const char* RETIRE_OFFERS =
    "UPDATE pending_intent SET state = 'expired', detail = 'replaced', updated_at = ? "
    "WHERE user_id = ? AND module = ? AND state = 'offered'";
inline constexpr const char* ACCEPT_OFFERS =
    "UPDATE pending_intent SET state = 'waiting', updated_at = ? "
    "WHERE user_id = ? AND module = ? AND state = 'offered' RETURNING id";
inline constexpr const char* SELECT_COLUMNS =
    "SELECT id, user_id, role, module, tool, arguments, lang, utterance, session_id, state, detail, created_at, "
    "updated_at FROM pending_intent ";
inline constexpr const char* WHERE_ID = "WHERE id = ?";
inline constexpr const char* WHERE_WAITING = "WHERE state = 'waiting' ORDER BY id";
inline constexpr const char* WHERE_WAITING_FOR_MODULE = "WHERE state = 'waiting' AND module = ? ORDER BY id";
inline constexpr const char* WHERE_WAITING_BEFORE = "WHERE state = 'waiting' AND updated_at < ? ORDER BY id";
inline constexpr const char* SETTLE =
    "UPDATE pending_intent SET state = ?, detail = ?, updated_at = ? WHERE id = ? AND state = 'waiting'";
inline constexpr const char* EXPIRE_OFFERS =
    "UPDATE pending_intent SET state = 'expired', detail = 'unanswered', updated_at = ? "
    "WHERE state = 'offered' AND updated_at < ?";
inline constexpr const char* PURGE_SETTLED =
    "DELETE FROM pending_intent WHERE state IN ('done', 'failed', 'expired') AND updated_at < ?";
}

struct PendingIntentCreateInput
{
  int64_t userId{0};
  std::string role;
  std::string module;
  std::string tool;
  std::string arguments;
  std::string lang;
  std::string utterance;
  std::string sessionId;
  int64_t at{0};
};

struct PendingIntentAcceptInput
{
  int64_t userId{0};
  std::string module;
  int64_t at{0};
};

struct PendingIntentSettleInput
{
  int64_t id{0};
  PendingIntentState state{PendingIntentState::Done};
  std::string detail;
  int64_t at{0};
};

struct PendingIntentRow
{
  int64_t id{0};
  int64_t userId{0};
  std::string role;
  std::string module;
  std::string tool;
  std::string arguments;
  std::string lang;
  std::string utterance;
  std::string sessionId;
  PendingIntentState state{PendingIntentState::Offered};
  std::string detail;
  int64_t createdAt{0};
  int64_t updatedAt{0};
};
