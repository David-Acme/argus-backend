#pragma once
#include <camera/camera-driver.hxx>
#include <camera/camera-record-mode.hxx>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <optional>
#include <string>
#include <string_view>

namespace camera_query
{
inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM camera WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND_ENABLED =
    "SELECT * FROM camera WHERE deleted_at IS NULL AND is_enabled = 1 "
    "ORDER BY id ASC";
inline constexpr std::string_view FIND_LIVE =
    "SELECT * FROM camera WHERE deleted_at IS NULL ORDER BY id ASC";
inline constexpr std::string_view FIND =
    "SELECT * FROM camera WHERE deleted_at IS NULL "
    "AND created_at >= ? AND created_at <= ? ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM camera WHERE deleted_at IS NULL "
    "AND created_at >= ? ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER =
    "SELECT * FROM camera WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) AND created_at <= ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER_FROM =
    "SELECT * FROM camera WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM camera WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? AND deleted_at <= ? ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM camera WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER =
    "SELECT * FROM camera WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) AND deleted_at <= ? "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER_FROM =
    "SELECT * FROM camera WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM camera WHERE deleted_at IS NULL "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM camera WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM camera WHERE deleted_at IS NULL ORDER BY created_at DESC "
    "LIMIT 1";
inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM camera WHERE deleted_at IS NOT NULL ORDER BY deleted_at "
    "DESC LIMIT 1";
inline constexpr std::string_view INSERT =
    "INSERT INTO camera (name, manufacturer, model, ip, port, username, "
    "password, cloud_username, cloud_password, driver, icon, record_mode, "
    "retention_days, capabilities, config, is_enabled) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
inline constexpr std::string_view UPDATE_PREFIX = "UPDATE camera SET ";
inline constexpr std::string_view UPDATE_COL_NAME = "name = ?";
inline constexpr std::string_view UPDATE_COL_MANUFACTURER = "manufacturer = ?";
inline constexpr std::string_view UPDATE_COL_MODEL = "model = ?";
inline constexpr std::string_view UPDATE_COL_IP = "ip = ?";
inline constexpr std::string_view UPDATE_COL_PORT = "port = ?";
inline constexpr std::string_view UPDATE_COL_USERNAME = "username = ?";
inline constexpr std::string_view UPDATE_COL_PASSWORD = "password = ?";
inline constexpr std::string_view UPDATE_COL_CLOUD_USERNAME = "cloud_username = ?";
inline constexpr std::string_view UPDATE_COL_CLOUD_PASSWORD = "cloud_password = ?";
inline constexpr std::string_view UPDATE_COL_DRIVER = "driver = ?";
inline constexpr std::string_view UPDATE_COL_ICON = "icon = ?";
inline constexpr std::string_view UPDATE_COL_RECORD_MODE = "record_mode = ?";
inline constexpr std::string_view UPDATE_COL_RETENTION_DAYS =
    "retention_days = ?";
inline constexpr std::string_view UPDATE_COL_CAPABILITIES = "capabilities = ?";
inline constexpr std::string_view UPDATE_COL_CONFIG = "config = ?";
inline constexpr std::string_view UPDATE_COL_IS_ENABLED = "is_enabled = ?";
inline constexpr std::string_view UPDATE_COL_IS_ONLINE = "is_online = ?";
inline constexpr std::string_view UPDATE_COL_RESET_TRUST =
    "tls_fingerprint = '', tapo_secure = 0";
inline constexpr std::string_view UPDATE_SUFFIX =
    ", updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND_DELETED_BOUNDARY =
    "SELECT * FROM camera WHERE deleted_at IS NOT NULL AND deleted_at = ? "
    "AND id <= ? ORDER BY id ASC LIMIT 200";
inline constexpr std::string_view TABLE_COLUMNS =
    "SELECT name FROM pragma_table_info('camera')";
inline constexpr std::string_view ADD_TLS_FINGERPRINT =
    "ALTER TABLE camera ADD COLUMN tls_fingerprint TEXT NOT NULL DEFAULT ''";
inline constexpr std::string_view ADD_TAPO_SECURE =
    "ALTER TABLE camera ADD COLUMN tapo_secure INTEGER NOT NULL DEFAULT 0";
inline constexpr std::string_view PLAINTEXT_SECRETS =
    "SELECT id, password, cloud_password FROM camera "
    "WHERE (password != '' AND password NOT LIKE 'enc:v1:%') "
    "OR (cloud_password != '' AND cloud_password NOT LIKE 'enc:v1:%')";
inline constexpr std::string_view SEAL_SECRETS =
    "UPDATE camera SET password = ?, cloud_password = ? WHERE id = ?";
inline constexpr std::string_view SAVE_TAPO_TRUST =
    "UPDATE camera SET tls_fingerprint = ?, tapo_secure = ? WHERE id = ?";
inline constexpr std::string_view REMOVE =
    "UPDATE camera SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL";
}

struct CameraCreateInput
{
  std::string name;
  std::string manufacturer;
  std::string model;
  std::string ip;
  int32_t port{554};
  std::string username;
  std::string password;
  std::string cloudUsername;
  std::string cloudPassword;
  CameraDriver driver{CameraDriver::Tapo};
  std::string icon{"video"};
  CameraRecordMode recordMode{CameraRecordMode::Events};
  std::optional<int64_t> retentionDays;
  std::string capabilities;
  std::string config;
  drogon::orm::DbClient* client{nullptr};
};

struct CameraTapoTrustInput
{
  int64_t cameraId{0};
  std::string fingerprint;
  bool secure{false};
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
  std::optional<std::string> cloudUsername;
  std::optional<std::string> cloudPassword;
  std::optional<CameraDriver> driver;
  std::optional<std::string> icon;
  std::optional<CameraRecordMode> recordMode;
  std::optional<int64_t> retentionDays;
  std::optional<std::string> capabilities;
  std::optional<std::string> config;
  std::optional<bool> isEnabled;
  std::optional<bool> isOnline;
  bool resetTapoTrust{false};
  drogon::orm::DbClient* client{nullptr};
};
