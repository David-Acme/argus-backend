#pragma once
#include <cstdint>
#include <shared/enums.hxx>
#include <string>
#include <string_view>

namespace fallback_log_query
{
inline constexpr std::string_view INSERT_FALLBACK_EVENT =
    "INSERT INTO gateway_fallback_event (camera_id, rule, severity, reason, "
    "created_at) VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view PURGE_FALLBACK_EVENTS =
    "DELETE FROM gateway_fallback_event WHERE created_at < ?";
} // namespace fallback_log_query

struct FallbackLogInput
{
  int64_t cameraId{0};
  std::string rule;
  std::string severity;
  FallbackDropReason reason{FallbackDropReason::NonHardSignal};
  int64_t createdAt{0};
};
