#pragma once

#include <feature/camera-control/services/patrol-plan.hxx>

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace camera_patrol_query
{
inline constexpr std::string_view FIND_BY_CAMERA =
    "SELECT * FROM camera_patrol WHERE camera_id = ?";
inline constexpr std::string_view FIND_ACTIVE =
    "SELECT p.*, c.is_online AS camera_online FROM camera_patrol p "
    "JOIN camera c ON c.id = p.camera_id "
    "WHERE p.is_enabled = 1 AND c.deleted_at IS NULL AND c.is_enabled = 1 "
    "ORDER BY p.camera_id ASC";
inline constexpr std::string_view INSERT =
    "INSERT INTO camera_patrol (camera_id, stops, dwell_seconds, is_enabled, start_hour, end_hour) "
    "VALUES (?, ?, ?, ?, ?, ?)";
inline constexpr std::string_view UPDATE_PREFIX = "UPDATE camera_patrol SET ";
inline constexpr std::string_view UPDATE_COL_STOPS = "stops = ?";
inline constexpr std::string_view UPDATE_COL_DWELL_SECONDS = "dwell_seconds = ?";
inline constexpr std::string_view UPDATE_COL_IS_ENABLED = "is_enabled = ?";
inline constexpr std::string_view UPDATE_COL_WINDOW = "start_hour = ?, end_hour = ?";
inline constexpr std::string_view UPDATE_COL_NO_WINDOW = "start_hour = NULL, end_hour = NULL";
inline constexpr std::string_view UPDATE_SUFFIX =
    ", updated_at = strftime('%s', 'now') WHERE camera_id = ?";
inline constexpr std::string_view REMOVE = "DELETE FROM camera_patrol WHERE camera_id = ?";
}

struct CameraPatrolCreateInput
{
  int64_t cameraId{0};
  std::vector<std::string> stops;
  int64_t dwellSeconds{patrol_plan::kDefaultDwellSeconds};
  bool isEnabled{true};
  std::optional<PatrolWindow> window;
  drogon::orm::DbClient* client{nullptr};
};

struct CameraPatrolUpdateInput
{
  std::optional<std::vector<std::string>> stops;
  std::optional<int64_t> dwellSeconds;
  std::optional<bool> isEnabled;
  std::optional<std::optional<PatrolWindow>> window;
  drogon::orm::DbClient* client{nullptr};
};
