#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace proximity_beacon_query
{

inline constexpr std::string_view SELECT_BEACON =
    "SELECT user_id, seed, enabled, updated_at FROM guard_proximity_beacon WHERE user_id = ?";

inline constexpr std::string_view SELECT_ENABLED =
    "SELECT user_id, seed, enabled, updated_at FROM guard_proximity_beacon WHERE enabled = 1";

inline constexpr std::string_view UPSERT_BEACON =
    "INSERT INTO guard_proximity_beacon (user_id, seed, enabled, created_at, updated_at) "
    "VALUES (?, ?, ?, ?, ?) ON CONFLICT(user_id) DO UPDATE SET "
    "seed = excluded.seed, enabled = excluded.enabled, updated_at = excluded.updated_at";

inline constexpr std::string_view DISABLE_BEACON =
    "UPDATE guard_proximity_beacon SET enabled = 0, updated_at = ? WHERE user_id = ?";

}

struct ProximityBeaconRow
{
  int64_t userId{0};
  std::string seed;
  bool enabled{false};
  int64_t updatedAt{0};
};

struct ProximityBeaconUpsertInput
{
  int64_t userId{0};
  std::string seed;
  bool enabled{false};
  int64_t now{0};
};
