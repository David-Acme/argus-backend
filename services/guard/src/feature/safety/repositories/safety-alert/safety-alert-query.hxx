#pragma once

#include <feature/safety/vocabulary/safety-alert-kind.hxx>

#include <cstdint>
#include <string>
#include <string_view>

namespace safety_alert_query
{

inline constexpr std::string_view INSERT_ALERT =
    "INSERT INTO guard_safety_alert (kind, user_id, environment_id, created_at, actor_name) "
    "VALUES (?, ?, ?, ?, ?) RETURNING id";

inline constexpr std::string_view RECENT_ALERT =
    "SELECT id, notified_at FROM guard_safety_alert WHERE kind = ? AND "
    "user_id = ? AND created_at >= ? ORDER BY id DESC LIMIT 1";

inline constexpr std::string_view COUNT_SINCE =
    "SELECT COUNT(*) AS total FROM guard_safety_alert WHERE kind = ? AND "
    "user_id = ? AND created_at >= ?";

inline constexpr std::string_view PENDING_ALERTS =
    "SELECT id, kind, user_id, environment_id, created_at, notify_sequence, escalated_at, "
    "actor_name "
    "FROM guard_safety_alert WHERE notified_at = 0 ORDER BY id DESC LIMIT 200";

inline constexpr std::string_view ADVANCE_SEQUENCE =
    "UPDATE guard_safety_alert SET notify_sequence = notify_sequence + 1 "
    "WHERE id = ? AND notify_sequence = ? AND notified_at = 0";

inline constexpr std::string_view MARK_ESCALATED =
    "UPDATE guard_safety_alert SET escalated_at = ? WHERE id = ? AND escalated_at = 0";

inline constexpr std::string_view MARK_NOTIFIED =
    "UPDATE guard_safety_alert SET notified_at = ? WHERE id = ?";

inline constexpr std::string_view PURGE_BEFORE =
    "DELETE FROM guard_safety_alert WHERE created_at < ? AND notified_at > 0";

}

struct SafetyAlertInsertInput
{
  SafetyAlertKind kind{SafetyAlertKind::Panic};
  int64_t userId{0};
  int64_t environmentId{0};
  int64_t now{0};
  std::string actorName;
};

struct SafetyAlertRow
{
  int64_t id{0};
  SafetyAlertKind kind{SafetyAlertKind::Panic};
  int64_t userId{0};
  int64_t environmentId{0};
  int64_t createdAt{0};
  int64_t sequence{1};
  int64_t escalatedAt{0};
  std::string actorName;
};

struct SafetyAlertSequence
{
  int64_t id{0};
  int64_t sequence{1};
};

struct SafetyAlertRecent
{
  int64_t id{0};
  bool notified{false};
};

struct SafetyAlertRecentInput
{
  SafetyAlertKind kind{SafetyAlertKind::Panic};
  int64_t userId{0};
  int64_t since{0};
};
