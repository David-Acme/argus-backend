#include "guard-module-impact.hxx"

#include <auth/role-access.hxx>

namespace
{
constexpr std::string_view kPendingAlerts = "pending_alerts";
constexpr std::string_view kGuardDuty = "guard_duty";
}

ModuleImpactReport GuardModuleImpact::impact(const std::string& moduleId) const
{
  return drogon::sync_wait(impactAsync(moduleId));
}

drogon::Task<ModuleImpactReport> GuardModuleImpact::impactAsync(std::string moduleId) const
{
  ModuleImpactReport report;
  if (moduleId != role_access::kSurveillanceModule)
    co_return report;
  const auto pending = co_await repository_.pending();
  report.stops.push_back({.kind = std::string(kPendingAlerts), .count = pending.observations + pending.actions});
  report.stops.push_back({.kind = std::string(kGuardDuty), .count = pending.duty});
  co_return report;
}
