#pragma once

#include <array>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <optional>
#include <string>
#include <string_view>
#include <camera/zone-type.hxx>

namespace zone_query
{
inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM zone WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND_BY_CAMERA =
    "SELECT * FROM zone WHERE camera_id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND =
    "SELECT * FROM zone WHERE deleted_at IS NULL "
    "AND created_at >= ? AND created_at <= ? ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM zone WHERE deleted_at IS NULL "
    "AND created_at >= ? ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER =
    "SELECT * FROM zone WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) AND created_at <= ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER_FROM =
    "SELECT * FROM zone WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM zone WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? AND deleted_at <= ? ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM zone WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER =
    "SELECT * FROM zone WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) AND deleted_at <= ? "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER_FROM =
    "SELECT * FROM zone WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM zone WHERE deleted_at IS NULL "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM zone WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
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
inline constexpr std::string_view ZONE_TABLE_SQL =
    "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'zone'";
inline constexpr std::array<std::string_view, 7> ZONE_REBUILD = {
    "CREATE TABLE zone_rebuild ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "camera_id INTEGER NOT NULL REFERENCES camera(id) ON DELETE CASCADE, "
    "name TEXT NOT NULL, points TEXT NOT NULL, "
    "zone_type TEXT NOT NULL DEFAULT 'monitor' "
    "CHECK (zone_type IN ('monitor', 'alert', 'exclude', 'privacy')), "
    "color TEXT NOT NULL DEFAULT '#FF0000', "
    "is_enabled INTEGER NOT NULL DEFAULT 1, "
    "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
    "updated_at INTEGER, deleted_at INTEGER)",
    "INSERT INTO zone_rebuild (id, camera_id, name, points, zone_type, color, "
    "is_enabled, created_at, updated_at, deleted_at) "
    "SELECT id, camera_id, name, points, zone_type, color, is_enabled, "
    "created_at, updated_at, deleted_at FROM zone",
    "DROP TABLE zone",
    "ALTER TABLE zone_rebuild RENAME TO zone",
    "CREATE INDEX IF NOT EXISTS idx_zone_camera_id ON zone (camera_id)",
    "CREATE INDEX IF NOT EXISTS idx_zone_created_at ON zone (created_at)",
    "CREATE INDEX IF NOT EXISTS idx_zone_deleted_at ON zone (deleted_at)"};

inline constexpr std::string_view REMOVE =
    "UPDATE zone SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL";
}

struct ZoneCreateInput
{
  int64_t cameraId{0};
  std::string name;
  std::string points;
  ZoneType zoneType{ZoneType::Monitor};
  std::string color;
  bool isEnabled{true};
  drogon::orm::DbClient* client{nullptr};
};

struct ZoneUpdateInput
{
  std::optional<std::string> name;
  std::optional<std::string> points;
  std::optional<ZoneType> zoneType;
  std::optional<std::string> color;
  std::optional<bool> isEnabled;
  drogon::orm::DbClient* client{nullptr};
};
