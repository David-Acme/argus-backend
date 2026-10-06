#pragma once

#include <settings/component-vocabulary.hxx>

class ModuleRequestHost
{
public:
  ModuleRequestHost() = default;
  virtual ~ModuleRequestHost() = default;
  ModuleRequestHost(const ModuleRequestHost&) = delete;
  ModuleRequestHost& operator=(const ModuleRequestHost&) = delete;
  ModuleRequestHost(ModuleRequestHost&&) = delete;
  ModuleRequestHost& operator=(ModuleRequestHost&&) = delete;

  virtual ModuleRequestOutcome request(const ModuleRequestInput& input) = 0;
};
