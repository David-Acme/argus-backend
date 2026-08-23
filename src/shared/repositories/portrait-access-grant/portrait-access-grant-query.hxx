#pragma once

#include <cstdint>
#include <optional>
#include <shared/enums.hxx>
#include <string_view>
#include <vector>

namespace portrait_access_grant_query
{

inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM portrait_access_grant WHERE id = ?";

inline constexpr std::string_view FIND_ACTIVE =
    "SELECT * FROM portrait_access_grant "
    "WHERE portrait_user_id = ? AND grantee_user_id = ? "
    "AND revoked_at IS NULL AND (expires_at IS NULL OR expires_at > ?) "
    "ORDER BY created_at DESC LIMIT 1";

inline constexpr std::string_view FIND_ACTIVE_FOR_GRANTEE =
    "SELECT * FROM portrait_access_grant "
    "WHERE grantee_user_id = ? AND revoked_at IS NULL "
    "AND (expires_at IS NULL OR expires_at > ?) "
    "ORDER BY created_at DESC, id DESC";

inline constexpr std::string_view INSERT =
    "INSERT INTO portrait_access_grant "
    "(request_id, portrait_user_id, grantee_user_id, granted_by, scope, expires_at) "
    "VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view REVOKE =
    "UPDATE portrait_access_grant "
    "SET revoked_at = strftime('%s', 'now'), revoked_by = ?, "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND revoked_at IS NULL";

} // namespace portrait_access_grant_query

struct PortraitAccessGrantCreateInput
{
  std::optional<int64_t> requestId;
  int64_t portraitUserId{0};
  int64_t granteeUserId{0};
  int64_t grantedBy{0};
  PortraitAccessGrantScope scope{PortraitAccessGrantScope::Temporary};
  std::optional<int64_t> expiresAt;
};

struct PortraitAccessGrantFindActiveInput
{
  int64_t portraitUserId{0};
  int64_t granteeUserId{0};
  int64_t now{0};
};

struct PortraitAccessGrantRevokeInput
{
  int64_t grantId{0};
  int64_t revokedBy{0};
};
