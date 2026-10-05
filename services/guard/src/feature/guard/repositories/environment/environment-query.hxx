#pragma once

#include <feature/guard/vocabulary/environment-kind.hxx>
#include <feature/guard/vocabulary/quiet-policy.hxx>
#include <shared/vocabulary/guard-mode.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace environment_query
{

inline constexpr std::string_view LIST_ENVIRONMENTS =
    "SELECT id, name, kind, is_default, mode, mode_updated_at, "
    "schedule_enabled, asleep_hours, open_hours, staffed_hours, closed_mode, "
    "digest_hour, quiet_policy, quiet_start_hour, quiet_end_hour, lan_presence, "
    "created_at, updated_at FROM guard_environment ORDER BY is_default DESC, id ASC "
    "LIMIT 200";

inline constexpr std::string_view FIND_ENVIRONMENT =
    "SELECT id, name, kind, is_default, mode, mode_updated_at, "
    "schedule_enabled, asleep_hours, open_hours, staffed_hours, closed_mode, "
    "digest_hour, quiet_policy, quiet_start_hour, quiet_end_hour, lan_presence, "
    "created_at, updated_at FROM guard_environment WHERE id = ?";

inline constexpr std::string_view FOR_CAMERA =
    "SELECT e.id, e.name, e.kind, e.is_default, e.mode, e.mode_updated_at, "
    "e.schedule_enabled, e.asleep_hours, e.open_hours, e.staffed_hours, "
    "e.closed_mode, e.digest_hour, e.quiet_policy, e.quiet_start_hour, "
    "e.quiet_end_hour, e.lan_presence, e.created_at, e.updated_at, "
    "(SELECT COUNT(*) FROM guard_environment) AS total "
    "FROM guard_environment e WHERE e.id = COALESCE("
    "(SELECT c.environment_id FROM guard_camera_context c JOIN "
    "guard_environment x ON x.id = c.environment_id WHERE c.camera_id = ?), "
    "(SELECT d.id FROM guard_environment d WHERE d.is_default = 1))";

inline constexpr std::string_view COUNT_ENVIRONMENTS =
    "SELECT COUNT(*) AS total FROM guard_environment";

inline constexpr std::string_view DEFAULT_ID =
    "SELECT id FROM guard_environment WHERE is_default = 1";

inline constexpr std::string_view INSERT_ENVIRONMENT =
    "INSERT INTO guard_environment (name, kind, is_default, mode, "
    "mode_updated_at, schedule_enabled, asleep_hours, open_hours, "
    "staffed_hours, closed_mode, digest_hour, quiet_policy, quiet_start_hour, "
    "quiet_end_hour, lan_presence, created_at, updated_at) VALUES (?, ?, 0, ?, "
    "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id";

inline constexpr std::string_view UPDATE_ENVIRONMENT_PREFIX =
    "UPDATE guard_environment SET updated_at = ?";

inline constexpr std::string_view UPDATE_COL_NAME = ", name = ?";
inline constexpr std::string_view UPDATE_COL_KIND = ", kind = ?";
inline constexpr std::string_view UPDATE_COL_SCHEDULE_ENABLED =
    ", schedule_enabled = ?";
inline constexpr std::string_view UPDATE_COL_ASLEEP = ", asleep_hours = ?";
inline constexpr std::string_view UPDATE_COL_OPEN = ", open_hours = ?";
inline constexpr std::string_view UPDATE_COL_STAFFED = ", staffed_hours = ?";
inline constexpr std::string_view UPDATE_COL_CLOSED_MODE = ", closed_mode = ?";
inline constexpr std::string_view UPDATE_COL_DIGEST_HOUR = ", digest_hour = ?";
inline constexpr std::string_view UPDATE_COL_QUIET_POLICY =
    ", quiet_policy = ?";
inline constexpr std::string_view UPDATE_COL_QUIET_START =
    ", quiet_start_hour = ?";
inline constexpr std::string_view UPDATE_COL_QUIET_END = ", quiet_end_hour = ?";
inline constexpr std::string_view UPDATE_COL_LAN_PRESENCE =
    ", lan_presence = ?";

inline constexpr std::string_view UPDATE_ENVIRONMENT_SUFFIX = " WHERE id = ?";

inline constexpr std::string_view SET_MODE_ONE =
    "UPDATE guard_environment SET mode = ?, mode_updated_at = ? WHERE id = ?";

inline constexpr std::string_view SET_MODE_ALL =
    "UPDATE guard_environment SET mode = ?, mode_updated_at = ?";

inline constexpr std::string_view MOVE_CAMERAS =
    "UPDATE guard_camera_context SET environment_id = ?, updated_at = ? "
    "WHERE environment_id = ?";

inline constexpr std::string_view RETIRE_GUESTS =
    "UPDATE guard_expected_guest SET deleted_at = ? WHERE environment_id = ? "
    "AND deleted_at IS NULL";

inline constexpr std::string_view DELETE_ENVIRONMENT =
    "DELETE FROM guard_environment WHERE id = ? AND is_default = 0";

inline constexpr std::string_view CAMERA_ASSIGNMENTS =
    "SELECT c.camera_id, c.environment_id FROM guard_camera_context c "
    "JOIN guard_environment e ON e.id = c.environment_id "
    "ORDER BY c.camera_id ASC LIMIT 2000";

}

struct GuardEnvironment
{
  int64_t id{0};
  std::string name;
  EnvironmentKind kind{EnvironmentKind::Home};
  bool isDefault{false};
  GuardMode mode{GuardMode::Home};
  int64_t modeUpdatedAt{0};
  bool scheduleEnabled{false};
  std::string asleep;
  std::string open;
  std::string staffed;
  GuardMode closedMode{GuardMode::Away};
  int digestHour{21};
  QuietPolicy quietPolicy{QuietPolicy::Inherit};
  int quietStartHour{22};
  int quietEndHour{7};
  bool lanPresence{false};
  int64_t createdAt{0};
  int64_t updatedAt{0};
};

struct GuardEnvironmentScope
{
  GuardEnvironment environment;
  bool several{false};
};

struct EnvironmentCreateInput
{
  std::string name;
  EnvironmentKind kind{EnvironmentKind::Home};
  GuardMode mode{GuardMode::Home};
  bool scheduleEnabled{false};
  std::string asleep;
  std::string open;
  std::string staffed;
  GuardMode closedMode{GuardMode::Away};
  int digestHour{21};
  QuietPolicy quietPolicy{QuietPolicy::Inherit};
  int quietStartHour{22};
  int quietEndHour{7};
  bool lanPresence{false};
  int64_t at{0};
};

struct EnvironmentUpdateInput
{
  int64_t id{0};
  std::optional<std::string> name;
  std::optional<EnvironmentKind> kind;
  std::optional<bool> scheduleEnabled;
  std::optional<std::string> asleep;
  std::optional<std::string> open;
  std::optional<std::string> staffed;
  std::optional<GuardMode> closedMode;
  std::optional<int> digestHour;
  std::optional<QuietPolicy> quietPolicy;
  std::optional<int> quietStartHour;
  std::optional<int> quietEndHour;
  std::optional<bool> lanPresence;
  int64_t at{0};
};

struct EnvironmentModeInput
{
  std::optional<int64_t> environmentId;
  GuardMode mode{GuardMode::Home};
  int64_t at{0};
};

struct EnvironmentRemoveInput
{
  int64_t id{0};
  int64_t at{0};
};

enum class EnvironmentRemoval : uint8_t
{
  Removed = 0,
  NotFound,
  IsDefault
};

struct CameraAssignment
{
  int64_t cameraId{0};
  int64_t environmentId{0};
};
