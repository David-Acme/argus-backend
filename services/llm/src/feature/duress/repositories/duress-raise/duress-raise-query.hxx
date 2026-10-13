#pragma once

#include <auth/user-role.hxx>

#include <cstdint>
#include <string>

namespace duress_raise_query
{
inline constexpr const char* INSERT =
    "INSERT INTO duress_raise_outbox (user_id, role, key_digest, said_at) VALUES (?, ?, ?, ?) "
    "ON CONFLICT (user_id) DO NOTHING";
inline constexpr const char* SELECT_ID_OF_USER = "SELECT id FROM duress_raise_outbox WHERE user_id = ?";
inline constexpr const char* SELECT_ALL =
    "SELECT id, user_id, role, key_digest, said_at FROM duress_raise_outbox ORDER BY id";
inline constexpr const char* DELETE_BY_ID = "DELETE FROM duress_raise_outbox WHERE id = ?";
inline constexpr const char* SYNCHRONOUS_FULL = "PRAGMA synchronous = FULL";
inline constexpr const char* SYNCHRONOUS = "PRAGMA synchronous";
}

struct DuressRaiseCreateInput
{
  int64_t userId{0};
  UserRole role{UserRole::Unknown};
  std::string keyDigest;
  int64_t saidAt{0};
};

struct DuressRaiseRow
{
  int64_t id{0};
  int64_t userId{0};
  UserRole role{UserRole::Unknown};
  std::string keyDigest;
  int64_t saidAt{0};
};
