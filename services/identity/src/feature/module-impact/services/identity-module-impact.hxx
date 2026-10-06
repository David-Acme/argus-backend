#pragma once

#include <drogon/utils/coroutine.h>
#include <settings/module-impact-host.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>

#include <string>

class IdentityModuleImpact final : public ModuleImpactHost
{
public:
  [[nodiscard]] ModuleImpactReport impact(const std::string& moduleId) const override;
  [[nodiscard]] drogon::Task<ModuleImpactReport> impactAsync(std::string moduleId) const;

private:
  UserRepository userRepository_;
  UserInvitationRepository invitationRepository_;
};
