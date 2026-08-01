#pragma once
#include <cstdint>
#include <optional>
#include <shared/enums.hxx>
#include <string>
#include <string_view>

namespace zone_query
{
inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM zone WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND_BY_CAMERA =
    "SELECT * FROM zone WHERE camera_id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND =
    "SELECT * FROM zone WHERE deleted_at IS NULL "
    "AND created_at >= ? AND created_at <= ? ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM zone WHERE deleted_at IS NULL "
    "AND created_at >= ? ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM zone WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? AND deleted_at <= ? ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM zone WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM zone WHERE deleted_at IS NULL "
    "ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM zone WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM zone WHERE deleted_at IS NULL ORDER BY created_at DESC "
    "LIMIT 1";
inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM zone WHERE deleted_at IS NOT NULL ORDER BY deleted_at DESC "
    "LIMIT 1";
inline constexpr std::string_view INSERT =
    "INSERT INTO zone (camera_id, name, points, zone_type, color, is_enabled) "
    "VALUES (?, ?, ?, ?, ?, ?)";
inline constexpr std::string_view UPDATE_PREFIX = "UPDATE zone SET ";
inline constexpr std::string_view UPDATE_COL_NAME = "name = ?";
inline constexpr std::string_view UPDATE_COL_POINTS = "points = ?";
inline constexpr std::string_view UPDATE_COL_ZONE_TYPE = "zone_type = ?";
inline constexpr std::string_view UPDATE_COL_COLOR = "color = ?";
inline constexpr std::string_view UPDATE_COL_IS_ENABLED = "is_enabled = ?";
inline constexpr std::string_view UPDATE_SUFFIX =
    ", updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view REMOVE =
    "UPDATE zone SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL";
} // namespace zone_query

struct ZoneCreateInput
{
  int64_t cameraId{0};
  std::string name;
  std::string points;
  ZoneType zoneType{ZoneType::Monitor};
  std::string color;
  bool isEnabled{true};
};

struct ZoneUpdateInput
{
  std::optional<std::string> name;
  std::optional<std::string> points;
  std::optional<ZoneType> zoneType;
  std::optional<std::string> color;
  std::optional<bool> isEnabled;
};
