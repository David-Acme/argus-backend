#pragma once

#include <cstdint>
#include <shared/enums.hxx>
#include <string>
#include <string_view>

namespace user_invitation_query
{

inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM user_invitation WHERE id = ?";

inline constexpr std::string_view FIND_BY_TOKEN_HASH =
    "SELECT * FROM user_invitation WHERE token_hash = ?";

inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM user_invitation ORDER BY created_at DESC, id DESC LIMIT 200";

inline constexpr std::string_view FIND_SYNC =
    "SELECT * FROM user_invitation WHERE COALESCE(updated_at, created_at) >= ? "
    "AND COALESCE(updated_at, created_at) <= ? "
    "ORDER BY COALESCE(updated_at, created_at) ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_SYNC_FROM =
    "SELECT * FROM user_invitation WHERE COALESCE(updated_at, created_at) >= ? "
    "ORDER BY COALESCE(updated_at, created_at) ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_SYNC_ALL =
    "SELECT * FROM user_invitation ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_SYNC_AFTER =
    "SELECT * FROM user_invitation WHERE "
    "(COALESCE(updated_at, created_at) > ? OR "
    "(COALESCE(updated_at, created_at) = ? AND id > ?)) "
    "AND COALESCE(updated_at, created_at) <= ? "
    "ORDER BY COALESCE(updated_at, created_at) ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_SYNC_AFTER_FROM =
    "SELECT * FROM user_invitation WHERE "
    "(COALESCE(updated_at, created_at) > ? OR "
    "(COALESCE(updated_at, created_at) = ? AND id > ?)) "
    "ORDER BY COALESCE(updated_at, created_at) ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_SYNC_LAST =
    "SELECT * FROM user_invitation "
    "ORDER BY COALESCE(updated_at, created_at) DESC, id DESC LIMIT 1";

inline constexpr std::string_view INSERT =
    "INSERT INTO user_invitation "
    "(token_hash, role, max_redemptions, expires_at, created_by) "
    "VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view REVOKE =
    "UPDATE user_invitation "
    "SET revoked_at = strftime('%s', 'now'), revoked_by = ?, "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND revoked_at IS NULL";

inline constexpr std::string_view TRY_CONSUME =
    "UPDATE user_invitation "
    "SET redemption_count = redemption_count + 1, "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND revoked_at IS NULL AND expires_at > ? "
    "AND redemption_count < max_redemptions";

inline constexpr std::string_view INSERT_REDEMPTION =
    "INSERT INTO invitation_redemption (invitation_id, user_id) VALUES (?, ?)";

} // namespace user_invitation_query

struct UserInvitationCreateInput
{
  std::string tokenHash;
  UserRole role{UserRole::Guest};
  int maxRedemptions{1};
  int64_t expiresAt{0};
  int64_t createdBy{0};
};

struct UserInvitationRevokeInput
{
  int64_t invitationId{0};
  int64_t revokedBy{0};
};

struct InvitationRedemptionCreateInput
{
  int64_t invitationId{0};
  int64_t userId{0};
};
