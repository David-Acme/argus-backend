#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace portrait_preview_capability_query
{
inline constexpr std::string_view FIND_BY_TOKEN_HASH =
    "SELECT * FROM portrait_preview_capability WHERE token_hash = ?";

inline constexpr std::string_view INSERT =
    "INSERT INTO portrait_preview_capability "
    "(token_hash, portrait_user_id, requester_user_id, expires_at) "
    "VALUES (?, ?, ?, ?)";

inline constexpr std::string_view TRY_CONSUME =
    "UPDATE portrait_preview_capability SET consumed_at = strftime('%s', 'now') "
    "WHERE id = ? AND requester_user_id = ? AND consumed_at IS NULL AND expires_at > ?";
} // namespace portrait_preview_capability_query

struct PortraitPreviewCapabilityCreateInput
{
  std::string tokenHash;
  int64_t portraitUserId{0};
  int64_t requesterUserId{0};
  int64_t expiresAt{0};
};

struct PortraitPreviewCapabilityConsumeInput
{
  int64_t id{0};
  int64_t requesterUserId{0};
  int64_t now{0};
};
