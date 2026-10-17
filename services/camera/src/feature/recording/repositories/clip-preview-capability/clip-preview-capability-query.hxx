#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>

namespace clip_preview_capability_query
{
inline constexpr std::string_view FIND_BY_TOKEN_HASH =
    "SELECT * FROM camera_clip_capability WHERE token_hash = ?";

inline constexpr std::string_view INSERT =
    "INSERT INTO camera_clip_capability "
    "(token_hash, clip_id, requester_user_id, expires_at) VALUES (?, ?, ?, ?)";

inline constexpr std::string_view TRY_CONSUME =
    "UPDATE camera_clip_capability SET consumed_at = strftime('%s', 'now') "
    "WHERE id = ? AND requester_user_id = ? AND consumed_at IS NULL "
    "AND expires_at > ?";

inline constexpr std::string_view PURGE_SPENT =
    "DELETE FROM camera_clip_capability "
    "WHERE expires_at <= ? OR consumed_at IS NOT NULL";
}

struct ClipPreviewCapabilityCreateInput
{
  std::string tokenHash;
  int64_t clipId{0};
  int64_t requesterUserId{0};
  int64_t expiresAt{0};
  drogon::orm::DbClient* client{nullptr};
};

struct ClipPreviewCapabilityConsumeInput
{
  int64_t id{0};
  int64_t requesterUserId{0};
  int64_t now{0};
  drogon::orm::DbClient* client{nullptr};
};
