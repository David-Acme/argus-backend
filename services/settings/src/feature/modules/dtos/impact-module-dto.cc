#include "impact-module-dto.hxx"

#include <validation/validation_dsl.hxx>

ImpactModuleDto ImpactModuleDto::fromAction(const std::string& action)
{
  ImpactModuleDto dto;
  dto.action = action;
  START_VALIDATION(ImpactModuleDto, dto)
  IS_IN(action, "disable", "uninstall")
  END_VALIDATION()
  dto.impactAction = impactActionFromString(dto.action).value_or(ImpactAction::Disable);
  return dto;
}
