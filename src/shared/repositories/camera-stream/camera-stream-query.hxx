#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace camera_stream_query
{
inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM camera_stream WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND_BY_CAMERA =
    "SELECT * FROM camera_stream WHERE camera_id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND =
    "SELECT * FROM camera_stream WHERE deleted_at IS NULL "
    "AND created_at >= ? AND created_at <= ? ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM camera_stream WHERE deleted_at IS NULL "
    "AND created_at >= ? ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM camera_stream WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? AND deleted_at <= ? ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM camera_stream WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM camera_stream WHERE deleted_at IS NULL "
    "ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM camera_stream WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM camera_stream WHERE deleted_at IS NULL ORDER BY created_at "
    "DESC LIMIT 1";
inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM camera_stream WHERE deleted_at IS NOT NULL ORDER BY "
    "deleted_at DESC LIMIT 1";
inline constexpr std::string_view INSERT =
    "INSERT INTO camera_stream (camera_id, label, url, resolution, fps, codec, "
    "is_primary, is_enabled) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
inline constexpr std::string_view UPDATE =
    "UPDATE camera_stream SET label = ?, url = ?, resolution = ?, fps = ?, "
    "codec = ?, is_primary = ?, is_enabled = ?, updated_at = strftime('%s', "
    "'now') "
    "WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view REMOVE =
    "UPDATE camera_stream SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL";
} // namespace camera_stream_query

struct CameraStreamCreateInput
{
  int64_t cameraId{0};
  std::string label;
  std::string url;
  std::string resolution;
  int32_t fps{0};
  std::string codec;
  bool isPrimary{true};
  bool isEnabled{true};
};

struct CameraStreamUpdateInput
{
  std::string label;
  std::string url;
  std::string resolution;
  int32_t fps{0};
  std::string codec;
  bool isPrimary{true};
  bool isEnabled{true};
};

