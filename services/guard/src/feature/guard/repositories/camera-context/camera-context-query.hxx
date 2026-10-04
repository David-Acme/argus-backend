#pragma once

#include <feature/guard/vocabulary/camera-role.hxx>

#include <cstdint>
#include <string>
#include <string_view>

namespace camera_context_query
{

inline constexpr std::string_view SELECT_CONTEXT =
    "SELECT camera_id, role, outdoor, public_area, active_hours, environment_id, "
    "updated_at FROM guard_camera_context WHERE camera_id = ?";

inline constexpr std::string_view LIST_CONTEXTS =
    "SELECT camera_id, role, outdoor, public_area, active_hours, environment_id, "
    "updated_at FROM guard_camera_context ORDER BY camera_id ASC LIMIT 2000";

inline constexpr std::string_view UPSERT_CONTEXT =
    "INSERT INTO guard_camera_context (camera_id, role, outdoor, public_area, "
    "active_hours, environment_id, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?) "
    "ON CONFLICT(camera_id) DO UPDATE SET role = excluded.role, "
    "outdoor = excluded.outdoor, public_area = excluded.public_area, "
    "active_hours = excluded.active_hours, "
    "environment_id = excluded.environment_id, "
    "updated_at = excluded.updated_at "
    "RETURNING camera_id, role, outdoor, public_area, active_hours, "
    "environment_id, updated_at";

}

struct GuardCameraContext
{
  int64_t cameraId{0};
  CameraRole role{CameraRole::Other};
  bool outdoor{false};
  bool publicArea{false};
  std::string activeHours;
  int64_t environmentId{0};
  bool configured{false};
  int64_t updatedAt{0};
};

struct CameraContextUpsertInput
{
  int64_t cameraId{0};
  CameraRole role{CameraRole::Other};
  bool outdoor{false};
  bool publicArea{false};
  std::string activeHours;
  int64_t environmentId{0};
  int64_t updatedAt{0};
};
