#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/guard/repositories/wind-down/wind-down-repository.hxx>
#include <settings/module-impact-host.hxx>

#include <string>

class GuardModuleImpact final : public ModuleImpactHost
{
public:
  [[nodiscard]] ModuleImpactReport impact(const std::string& moduleId) const override;
  [[nodiscard]] drogon::Task<ModuleImpactReport> impactAsync(std::string moduleId) const;

private:
  WindDownRepository repository_;
};
