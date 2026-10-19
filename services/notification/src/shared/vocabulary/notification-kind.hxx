#pragma once

#include <array>
#include <string_view>

namespace notification_kind
{
inline constexpr std::string_view kSurveillance = "surveillance";
inline constexpr std::string_view kProductivity = "productivity";

struct KindModule
{
  std::string_view kind;
  std::string_view module;
};

inline constexpr std::array<KindModule, 11> kModuleKinds = {{
    {.kind = "guard_episode", .module = kSurveillance},
    {.kind = "guard_tamper", .module = kSurveillance},
    {.kind = "guard_tamper_ask", .module = kSurveillance},
    {.kind = "maintenance_view_check", .module = kSurveillance},
    {.kind = "camera_maintenance_notice", .module = kSurveillance},
    {.kind = "guard_digest", .module = kSurveillance},
    {.kind = "guard_arrival", .module = kSurveillance},
    {.kind = "camera_fallback", .module = kSurveillance},
    {.kind = "camera_fallback_digest", .module = kSurveillance},
    {.kind = "camera_privacy_coverage", .module = kSurveillance},
    {.kind = "agenda_event", .module = kProductivity},
}};

[[nodiscard]] constexpr std::string_view moduleOf(std::string_view kind)
{
  for (const auto& entry : kModuleKinds)
    if (entry.kind == kind)
      return entry.module;
  return {};
}
}
