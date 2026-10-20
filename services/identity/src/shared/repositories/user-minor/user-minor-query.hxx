#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <optional>
#include <string_view>

namespace user_minor_query
{

inline constexpr std::string_view FIND_USER = "SELECT * FROM user_minor WHERE user_id = ?";

inline constexpr std::string_view FIND_ALL =
    "SELECT m.* FROM user_minor m JOIN user u ON u.id = m.user_id WHERE u.deleted_at IS NULL";

inline constexpr std::string_view UPSERT =
    "INSERT INTO user_minor (user_id, guardian_consent, guardian_id, set_by) VALUES (?, ?, ?, ?) "
    "ON CONFLICT(user_id) DO UPDATE SET guardian_consent = excluded.guardian_consent, "
    "guardian_id = excluded.guardian_id, set_by = excluded.set_by, set_at = strftime('%s', 'now')";

inline constexpr std::string_view REMOVE = "DELETE FROM user_minor WHERE user_id = ?";

}

struct UserMinorUpsertInput
{
  int64_t userId{0};
  bool guardianConsent{false};
  std::optional<int64_t> guardianId;
  int64_t setBy{0};
  drogon::orm::DbClient* client{nullptr};
};
