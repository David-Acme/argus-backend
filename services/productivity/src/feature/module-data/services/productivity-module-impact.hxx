#pragma once

#include <feature/module-data/repositories/module-data/module-data-repository.hxx>

#include <drogon/utils/coroutine.h>
#include <settings/module-impact-host.hxx>

#include <string>

class ProductivityModuleImpact final : public ModuleImpactHost
{
public:
  [[nodiscard]] ModuleImpactReport impact(const std::string& moduleId) const override;
  [[nodiscard]] drogon::Task<ModuleImpactReport> impactAsync(std::string moduleId) const;

private:
  ModuleDataRepository repository_;
};
