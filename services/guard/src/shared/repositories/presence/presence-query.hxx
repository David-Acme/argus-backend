#pragma once

#include <shared/vocabulary/presence-state.hxx>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace presence_query
{

inline constexpr std::string_view FOR_ENVIRONMENT =
    "SELECT user_id, environment_id, state, source, since, last_home_at, "
    "last_signal_at FROM guard_presence WHERE environment_id = ? "
    "ORDER BY user_id ASC LIMIT 5000";

inline constexpr std::string_view FOR_USER =
    "SELECT user_id, environment_id, state, source, since, last_home_at, "
    "last_signal_at FROM guard_presence WHERE user_id = ? "
    "ORDER BY environment_id ASC LIMIT 500";

inline constexpr std::string_view LIST_ALL =
    "SELECT user_id, environment_id, state, source, since, last_home_at, "
    "last_signal_at FROM guard_presence ORDER BY user_id ASC, "
    "environment_id ASC LIMIT 20000";

inline constexpr std::string_view FIND =
    "SELECT user_id, environment_id, state, source, since, last_home_at, "
    "last_signal_at FROM guard_presence WHERE user_id = ? AND "
    "environment_id = ?";

inline constexpr std::string_view UPSERT =
    "INSERT INTO guard_presence (user_id, environment_id, state, source, "
    "since, last_home_at, last_signal_at) SELECT ?, e.id, ?, ?, ?, ?, ? "
    "FROM guard_environment e WHERE e.id = ? "
    "ON CONFLICT (user_id, environment_id) DO UPDATE SET "
    "state = excluded.state, source = excluded.source, "
    "since = excluded.since, last_home_at = excluded.last_home_at, "
    "last_signal_at = excluded.last_signal_at";

inline constexpr std::string_view REMOVE_USER =
    "DELETE FROM guard_presence WHERE user_id = ? RETURNING environment_id";

inline constexpr std::string_view USER_IDS =
    "SELECT DISTINCT user_id FROM guard_presence ORDER BY user_id ASC "
    "LIMIT 5000";

inline constexpr std::string_view LAN_ENVIRONMENTS =
    "SELECT id FROM guard_environment WHERE lan_presence = 1 "
    "ORDER BY id ASC LIMIT 200";

inline constexpr std::string_view EXPIRE_HOME =
    "UPDATE guard_presence SET state = 'away', source = 'timeout', "
    "since = ?, last_signal_at = ? WHERE state = 'home' AND "
    "last_home_at < ? RETURNING user_id, environment_id, state, source, "
    "since, last_home_at, last_signal_at";

inline constexpr std::string_view PURGE_STALE =
    "DELETE FROM guard_presence WHERE last_signal_at < ? OR environment_id NOT IN "
    "(SELECT id FROM guard_environment)";

}

struct PresenceLookupInput
{
  int64_t environmentId{0};
  std::vector<int64_t> userIds;
};

struct PresenceKey
{
  int64_t userId{0};
  int64_t environmentId{0};
};

struct PresenceDecision
{
  PresenceRow row;
  bool write{false};
  bool changed{false};
};

struct PresenceTransition
{
  PresenceKey key;
  std::function<PresenceDecision(const std::optional<PresenceRow>&)> decide;
};

struct PresenceExpireInput
{
  int64_t homeBefore{0};
  int64_t at{0};
};
