#pragma once

#include <feature/call/vocabulary/call-mode.hxx>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace call_preference_query
{
inline constexpr std::string_view FIND =
    "SELECT * FROM call_preference WHERE user_id = ?";

inline constexpr std::string_view FIND_MANY_PREFIX =
    "SELECT * FROM call_preference WHERE user_id IN (";

inline constexpr std::string_view FIND_ARRIVAL_SUBSCRIBERS =
    "SELECT * FROM call_preference WHERE guard_arrival IN ('call', 'notify') "
    "ORDER BY user_id ASC";

inline constexpr std::string_view INSERT_DEFAULTS =
    "INSERT OR IGNORE INTO call_preference (user_id, updated_at) VALUES (?, ?)";

inline constexpr std::string_view UPDATE_PREFIX = "UPDATE call_preference SET ";
inline constexpr std::string_view UPDATE_SUFFIX = " WHERE user_id = ?";

inline constexpr std::string_view UPDATE_COL_ENABLED = "enabled = ?";
inline constexpr std::string_view UPDATE_COL_GUARD_CRITICAL = "guard_critical = ?";
inline constexpr std::string_view UPDATE_COL_GUARD_INTRUDER = "guard_intruder = ?";
inline constexpr std::string_view UPDATE_COL_GUARD_ESCALATION =
    "guard_escalation = ?";
inline constexpr std::string_view UPDATE_COL_GUARD_ARRIVAL = "guard_arrival = ?";
inline constexpr std::string_view UPDATE_COL_AGENDA = "agenda = ?";
inline constexpr std::string_view UPDATE_COL_ASSISTANT = "assistant = ?";
inline constexpr std::string_view UPDATE_COL_QUIET_START = "quiet_start_hour = ?";
inline constexpr std::string_view UPDATE_COL_QUIET_END = "quiet_end_hour = ?";
inline constexpr std::string_view UPDATE_COL_DND_UNTIL = "dnd_until = ?";
inline constexpr std::string_view UPDATE_COL_CRITICAL_BYPASS =
    "critical_bypass = ?";
inline constexpr std::string_view UPDATE_COL_MUTED = "muted_environments = ?";
inline constexpr std::string_view UPDATE_COL_UPDATED_AT = "updated_at = ?";
}

struct CallPreferenceUpdateInput
{
  int64_t userId{0};
  std::optional<bool> enabled;
  std::optional<CallMode> guardCritical;
  std::optional<CallMode> guardIntruder;
  std::optional<CallMode> guardEscalation;
  std::optional<CallMode> guardArrival;
  std::optional<CallMode> agenda;
  std::optional<CallMode> assistant;
  std::optional<int> quietStartHour;
  std::optional<int> quietEndHour;
  std::optional<int64_t> dndUntil;
  std::optional<bool> criticalBypass;
  std::optional<std::vector<int64_t>> mutedEnvironmentIds;
  int64_t updatedAt{0};
};
