#pragma once

#include <feature/modules/services/module-engine.hxx>

#include <string>

struct ImpactModuleDto
{
  std::string action;
  ImpactAction impactAction{ImpactAction::Disable};

  static ImpactModuleDto fromAction(const std::string& action);
};
