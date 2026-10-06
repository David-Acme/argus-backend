#pragma once

#include <settings/component-vocabulary.hxx>

#include <string>

class ModuleDataHost
{
public:
  ModuleDataHost() = default;
  virtual ~ModuleDataHost() = default;
  ModuleDataHost(const ModuleDataHost&) = delete;
  ModuleDataHost& operator=(const ModuleDataHost&) = delete;
  ModuleDataHost(ModuleDataHost&&) = delete;
  ModuleDataHost& operator=(ModuleDataHost&&) = delete;

  [[nodiscard]] virtual ModuleDataSummary summary(const std::string& moduleId) const = 0;
  virtual ModuleDataPurge purge(const std::string& moduleId) = 0;
};
