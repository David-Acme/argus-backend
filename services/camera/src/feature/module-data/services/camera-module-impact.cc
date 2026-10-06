#include "camera-module-impact.hxx"

#include <auth/role-access.hxx>

#include <utility>

namespace
{
constexpr std::string_view kCameraTalk = "camera_talk";
constexpr std::string_view kLiveViews = "live_views";
}

CameraModuleImpact::CameraModuleImpact(CameraModuleImpactInput input) : input_(std::move(input)) {}

ModuleImpactReport CameraModuleImpact::impact(const std::string& moduleId) const
{
  ModuleImpactReport report;
  if (moduleId != role_access::kSurveillanceModule)
    return report;
  if (input_.talkSessions)
    report.stops.push_back({.kind = std::string(kCameraTalk), .count = static_cast<std::int64_t>(input_.talkSessions())});
  if (input_.liveViews)
    report.stops.push_back({.kind = std::string(kLiveViews), .count = static_cast<std::int64_t>(input_.liveViews())});
  return report;
}
