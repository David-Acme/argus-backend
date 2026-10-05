#pragma once

#include <feature/safety/vocabulary/safety-alert-kind.hxx>

#include <cstdint>
#include <string_view>

namespace safety_alert_query
{

inline constexpr std::string_view INSERT_ALERT =
    "INSERT INTO guard_safety_alert (kind, user_id, environment_id, created_at) "
    "VALUES (?, ?, ?, ?) RETURNING id";

inline constexpr std::string_view RECENT_ALERT =
    "SELECT id, notified_at FROM guard_safety_alert WHERE kind = ? AND "
    "user_id = ? AND created_at >= ? ORDER BY id DESC LIMIT 1";

inline constexpr std::string_view COUNT_SINCE =
    "SELECT COUNT(*) AS total FROM guard_safety_alert WHERE kind = ? AND "
    "user_id = ? AND created_at >= ?";

inline constexpr std::string_view PENDING_ALERTS =
    "SELECT id, kind, user_id, environment_id, created_at FROM guard_safety_alert "
    "WHERE notified_at = 0 AND created_at >= ? ORDER BY id ASC LIMIT 50";

inline constexpr std::string_view MARK_NOTIFIED =
    "UPDATE guard_safety_alert SET notified_at = ? WHERE id = ?";

inline constexpr std::string_view PURGE_BEFORE =
    "DELETE FROM guard_safety_alert WHERE created_at < ?";

}

struct SafetyAlertInsertInput
{
  SafetyAlertKind kind{SafetyAlertKind::Panic};
  int64_t userId{0};
  int64_t environmentId{0};
  int64_t now{0};
};

struct SafetyAlertRow
{
  int64_t id{0};
  SafetyAlertKind kind{SafetyAlertKind::Panic};
  int64_t userId{0};
  int64_t environmentId{0};
  int64_t createdAt{0};
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
