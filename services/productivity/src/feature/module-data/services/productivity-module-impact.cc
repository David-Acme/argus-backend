#include "productivity-module-impact.hxx"

#include <ctime>
#include <sqlite/db-service.hxx>

namespace
{
constexpr std::string_view kProductivityModule = "productivity";
constexpr std::string_view kAgendaCalls = "agenda_calls";
}

ModuleImpactReport ProductivityModuleImpact::impact(const std::string& moduleId) const
{
  return drogon::sync_wait(impactAsync(moduleId));
}

drogon::Task<ModuleImpactReport> ProductivityModuleImpact::impactAsync(std::string moduleId) const
{
  ModuleImpactReport report;
  if (moduleId != kProductivityModule)
    co_return report;
  const auto client = DbService::productivityClient();
  const auto upcoming = co_await repository_.upcomingEvents(client.get(), static_cast<std::int64_t>(std::time(nullptr)));
  report.stops.push_back({.kind = std::string(kAgendaCalls), .count = upcoming});
  co_return report;
}
