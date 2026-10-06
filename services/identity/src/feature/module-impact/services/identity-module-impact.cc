#include "identity-module-impact.hxx"

#include <algorithm>
#include <auth/module-gate.hxx>
#include <ctime>
#include <map>

ModuleImpactReport IdentityModuleImpact::impact(const std::string& moduleId) const
{
  return drogon::sync_wait(impactAsync(moduleId));
}

drogon::Task<ModuleImpactReport> IdentityModuleImpact::impactAsync(std::string moduleId) const
{
  ModuleImpactReport report;
  const auto modules = moduleGate().current();
  const auto found = std::ranges::find(modules->modules(), moduleId, &ModuleFlag::id);
  if (found == modules->modules().end() || found->roles.empty())
    co_return report;

  const auto users = co_await userRepository_.findAll();
  std::map<int64_t, std::string> names;
  for (const auto& user : users) {
    names[user.id] = user.name;
    if (std::ranges::find(found->roles, userRoleToString(user.role)) != found->roles.end())
      report.roleHolders.push_back({.userId = user.id,
                                    .name = user.name,
                                    .lastName = user.lastName,
                                    .role = userRoleToString(user.role),
                                    .isActive = user.isActive});
  }

  const auto pending = co_await invitationRepository_.findPending(
      {.roles = found->roles, .now = static_cast<int64_t>(std::time(nullptr)), .client = nullptr});
  for (const auto& invitation : pending) {
    const auto creator = names.find(invitation.createdBy);
    report.invitations.push_back({.id = invitation.id,
                                  .role = userRoleToString(invitation.role),
                                  .createdBy = invitation.createdBy,
                                  .createdByName = creator == names.end() ? std::string() : creator->second,
                                  .expiresAt = invitation.expiresAt});
  }
  co_return report;
}
