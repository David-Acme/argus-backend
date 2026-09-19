#include "update-guard-mode-dto.hxx"

#include <shared/validation/validation_dsl.hxx>

UpdateGuardModeDto UpdateGuardModeDto::fromJson(const Json::Value& json)
{
  UpdateGuardModeDto dto;
  dto.mode = json.get("mode", "").asString();

  START_VALIDATION(UpdateGuardModeDto, dto)
  IS_NOT_EMPTY(mode)
  IS_IN(mode, "home", "away", "night", "armed")
  END_VALIDATION()
  return dto;
}
