#pragma once
#include <cstdint>
#include <feature/camera-notification/repositories/camera-fallback-log/fallback-drop-reason.hxx>
#include <string>
#include <string_view>

namespace camera_fallback_log_query
{
inline constexpr std::string_view INSERT_FALLBACK_EVENT =
    "INSERT INTO camera_fallback_event (camera_id, rule, severity, reason, "
    "created_at) VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view PURGE_FALLBACK_EVENTS =
    "DELETE FROM camera_fallback_event WHERE created_at < ?";
}

struct CameraFallbackLogInput
{
  int64_t cameraId{0};
  std::string rule;
  std::string severity;
  FallbackDropReason reason{FallbackDropReason::NonHardSignal};
  int64_t createdAt{0};
};
