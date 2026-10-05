#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace user_pin_query
{

inline constexpr std::string_view SELECT_PIN =
    "SELECT user_id, disarm_hash, duress_hash, updated_at FROM guard_user_pin "
    "WHERE user_id = ?";

inline constexpr std::string_view UPSERT_PIN =
    "INSERT INTO guard_user_pin (user_id, disarm_hash, duress_hash, created_at, "
    "updated_at) VALUES (?, ?, ?, ?, ?) ON CONFLICT(user_id) DO UPDATE SET "
    "disarm_hash = excluded.disarm_hash, duress_hash = excluded.duress_hash, "
    "updated_at = excluded.updated_at";

inline constexpr std::string_view DELETE_PIN =
    "DELETE FROM guard_user_pin WHERE user_id = ?";

inline constexpr std::string_view DELETE_ALL_PINS = "DELETE FROM guard_user_pin";

}

struct UserPinRow
{
  int64_t userId{0};
  std::string disarmHash;
  std::string duressHash;
  int64_t updatedAt{0};
};

struct UserPinUpsertInput
{
  int64_t userId{0};
  std::string disarmHash;
  std::string duressHash;
  int64_t now{0};
};
