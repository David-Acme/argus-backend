#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace scheduled_call_query
{
inline constexpr std::string_view INSERT =
    "INSERT OR IGNORE INTO scheduled_call (user_id, command_id, fire_at, topic, "
    "lang, created_at) VALUES (?, ?, ?, ?, ?, ?) RETURNING id";

inline constexpr std::string_view FIND_BY_COMMAND =
    "SELECT * FROM scheduled_call WHERE command_id = ?";

inline constexpr std::string_view COUNT_PENDING =
    "SELECT COUNT(*) AS total FROM scheduled_call "
    "WHERE user_id = ? AND state = 'pending'";

inline constexpr std::string_view FIND_DUE =
    "SELECT * FROM scheduled_call WHERE state = 'pending' AND fire_at <= ? "
    "ORDER BY fire_at ASC LIMIT ?";

inline constexpr std::string_view MARK_FIRED =
    "UPDATE scheduled_call SET state = 'fired', fired_at = ? "
    "WHERE id = ? AND state = 'pending'";
}

struct ScheduledCallCreateInput
{
  int64_t userId{0};
  std::string commandId;
  int64_t fireAt{0};
  std::string topic;
  std::string lang;
  int64_t createdAt{0};
};

struct ScheduledCallCreateResult
{
  int64_t id{0};
  bool duplicate{false};
  bool conflict{false};
};

struct ScheduledCallDueInput
{
  int64_t now{0};
  int limit{50};
};
