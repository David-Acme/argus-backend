#pragma once

#include <settings/component-vocabulary.hxx>

#include <string>

class ModuleImpactHost
{
public:
  ModuleImpactHost() = default;
  virtual ~ModuleImpactHost() = default;
  ModuleImpactHost(const ModuleImpactHost&) = delete;
  ModuleImpactHost& operator=(const ModuleImpactHost&) = delete;
  ModuleImpactHost(ModuleImpactHost&&) = delete;
  ModuleImpactHost& operator=(ModuleImpactHost&&) = delete;

  [[nodiscard]] virtual ModuleImpactReport impact(const std::string& moduleId) const = 0;
};
