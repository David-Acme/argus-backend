#pragma once

#include <string_view>

namespace evidence_query
{
inline constexpr std::string_view INSERT_EVIDENCE =
    "INSERT INTO camera_evidence (camera_id, object_key, content_type, "
    "created_at, expires_at) VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view CAMERA_RETENTION =
    "SELECT retention_days, config FROM camera WHERE id = ?";

inline constexpr std::string_view EXPIRED_EVIDENCE =
    "SELECT e.id, e.object_key FROM camera_evidence e "
    "LEFT JOIN camera c ON c.id = e.camera_id "
    "WHERE e.id > ? AND e.deleted_at = 0 AND ("
    "(e.expires_at > 0 AND e.expires_at <= ?) OR e.created_at <= ? OR "
    "(c.id IS NOT NULL AND e.created_at <= ? - 86400 * MIN("
    "COALESCE(c.retention_days, 7), "
    "CASE WHEN json_valid(c.config) AND json_extract(c.config, '$.retentionIncident') = 1 "
    "THEN 120 ELSE 60 END))) "
    "ORDER BY e.id ASC LIMIT 200";

inline constexpr std::string_view COUNT_EXPIRED =
    "SELECT COUNT(*) AS total FROM camera_evidence e "
    "LEFT JOIN camera c ON c.id = e.camera_id "
    "WHERE e.deleted_at = 0 AND ("
    "(e.expires_at > 0 AND e.expires_at <= ?) OR e.created_at <= ? OR "
    "(c.id IS NOT NULL AND e.created_at <= ? - 86400 * MIN("
    "COALESCE(c.retention_days, 7), "
    "CASE WHEN json_valid(c.config) AND json_extract(c.config, '$.retentionIncident') = 1 "
    "THEN 120 ELSE 60 END)))";

inline constexpr std::string_view MARK_EVIDENCE_DELETED =
    "UPDATE camera_evidence SET deleted_at = ? WHERE id = ?";
}
