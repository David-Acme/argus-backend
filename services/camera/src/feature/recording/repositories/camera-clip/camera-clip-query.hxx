#pragma once

#include <camera/camera-clip-trigger.hxx>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>

namespace camera_clip_query
{
inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM camera_clip WHERE id = ? AND deleted_at = 0";

inline constexpr std::string_view FIND_BY_CAMERA =
    "SELECT * FROM camera_clip WHERE camera_id = ? AND deleted_at = 0 "
    "AND (created_at < ? OR (created_at = ? AND id < ?)) "
    "ORDER BY created_at DESC, id DESC LIMIT ?";

inline constexpr std::string_view FIND_BY_CAMERA_AFTER =
    "SELECT * FROM camera_clip WHERE camera_id = ? AND deleted_at = 0 "
    "AND created_at >= ? ORDER BY created_at ASC, id ASC LIMIT ?";

inline constexpr std::string_view INSERT =
    "INSERT INTO camera_clip (camera_id, trigger, object_key, content_type, "
    "codec, started_at, duration_ms, byte_size, expires_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view MARK_DELETED =
    "UPDATE camera_clip SET deleted_at = ? WHERE id = ? AND deleted_at = 0";

inline constexpr std::string_view EXPIRED_CLIPS =
    "SELECT c.id, c.object_key FROM camera_clip c "
    "LEFT JOIN camera cam ON cam.id = c.camera_id "
    "WHERE c.id > ? AND c.deleted_at = 0 AND c.expires_at > 0 AND ("
    "c.expires_at <= ? OR (cam.id IS NOT NULL AND cam.retention_days IS NOT NULL "
    "AND c.created_at <= ? - 86400 * MIN(cam.retention_days, ?))) "
    "ORDER BY c.id ASC LIMIT ?";

inline constexpr std::string_view COUNT_EXPIRED_CLIPS =
    "SELECT COUNT(*) AS total FROM camera_clip c "
    "LEFT JOIN camera cam ON cam.id = c.camera_id "
    "WHERE c.deleted_at = 0 AND c.expires_at > 0 AND ("
    "c.expires_at <= ? OR (cam.id IS NOT NULL AND cam.retention_days IS NOT NULL "
    "AND c.created_at <= ? - 86400 * MIN(cam.retention_days, ?)))";

inline constexpr std::string_view CAMERA_CLIP_RETENTION =
    "SELECT retention_days FROM camera WHERE id = ?";

inline constexpr std::string_view PURGE_DELETED =
    "DELETE FROM camera_clip WHERE deleted_at > 0 AND deleted_at <= ?";
}

struct CameraClipCreateInput
{
  int64_t cameraId{0};
  CameraClipTrigger trigger{CameraClipTrigger::Event};
  std::string objectKey;
  std::string contentType{"video/mp4"};
  std::string codec;
  int64_t startedAt{0};
  int64_t durationMs{0};
  int64_t byteSize{0};
  int64_t expiresAt{0};
  drogon::orm::DbClient* client{nullptr};
};

struct CameraClipPageInput
{
  int64_t cameraId{0};
  int64_t beforeCreatedAt{0};
  int64_t beforeId{0};
  int64_t limit{50};
};

struct CameraClipAfterInput
{
  int64_t cameraId{0};
  int64_t sinceCreatedAt{0};
  int64_t limit{50};
};

struct CameraClipDeleteInput
{
  int64_t id{0};
  int64_t deletedAt{0};
  drogon::orm::DbClient* client{nullptr};
};

struct CameraClipSweepInput
{
  int64_t afterId{0};
  int64_t now{0};
  int64_t maxRetentionDays{90};
  int64_t batch{200};
};
