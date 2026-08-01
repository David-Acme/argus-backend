#pragma once
#include <cstdint>
#include <optional>
#include <shared/enums.hxx>
#include <string>
#include <string_view>

namespace camera_query
{
inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM camera WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND =
    "SELECT * FROM camera WHERE deleted_at IS NULL "
    "AND created_at >= ? AND created_at <= ? ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM camera WHERE deleted_at IS NULL "
    "AND created_at >= ? ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM camera WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? AND deleted_at <= ? ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM camera WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM camera WHERE deleted_at IS NULL "
    "ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM camera WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM camera WHERE deleted_at IS NULL ORDER BY created_at DESC "
    "LIMIT 1";
inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM camera WHERE deleted_at IS NOT NULL ORDER BY deleted_at "
    "DESC LIMIT 1";
inline constexpr std::string_view INSERT =
    "INSERT INTO camera (name, manufacturer, model, ip, port, username, "
    "password, record_mode, retention_days, capabilities, config, is_enabled) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
inline constexpr std::string_view UPDATE_PREFIX = "UPDATE camera SET ";
inline constexpr std::string_view UPDATE_COL_NAME = "name = ?";
inline constexpr std::string_view UPDATE_COL_MANUFACTURER = "manufacturer = ?";
inline constexpr std::string_view UPDATE_COL_MODEL = "model = ?";
inline constexpr std::string_view UPDATE_COL_IP = "ip = ?";
inline constexpr std::string_view UPDATE_COL_PORT = "port = ?";
inline constexpr std::string_view UPDATE_COL_USERNAME = "username = ?";
inline constexpr std::string_view UPDATE_COL_PASSWORD = "password = ?";
inline constexpr std::string_view UPDATE_COL_RECORD_MODE = "record_mode = ?";
inline constexpr std::string_view UPDATE_COL_RETENTION_DAYS =
    "retention_days = ?";
inline constexpr std::string_view UPDATE_COL_CAPABILITIES = "capabilities = ?";
inline constexpr std::string_view UPDATE_COL_CONFIG = "config = ?";
inline constexpr std::string_view UPDATE_COL_IS_ENABLED = "is_enabled = ?";
inline constexpr std::string_view UPDATE_COL_IS_ONLINE = "is_online = ?";
inline constexpr std::string_view UPDATE_SUFFIX =
    ", updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view REMOVE =
    "UPDATE camera SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL";
} // namespace camera_query

struct CameraCreateInput
{
  std::string name;
  std::string manufacturer;
  std::string model;
  std::string ip;
  int32_t port{554};
  std::string username;
  std::string password;
  CameraRecordMode recordMode{CameraRecordMode::Events};
  std::optional<int64_t> retentionDays;
  std::string capabilities;
  std::string config;
};

struct CameraUpdateInput
{
  std::optional<std::string> name;
  std::optional<std::string> manufacturer;
  std::optional<std::string> model;
  std::optional<std::string> ip;
  std::optional<int32_t> port;
  std::optional<std::string> username;
  std::optional<std::string> password;
  std::optional<CameraRecordMode> recordMode;
  std::optional<int64_t> retentionDays;
  std::optional<std::string> capabilities;
  std::optional<std::string> config;
  std::optional<bool> isEnabled;
  std::optional<bool> isOnline;
};
