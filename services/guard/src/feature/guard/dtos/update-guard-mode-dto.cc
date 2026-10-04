#include "update-guard-mode-dto.hxx"

#include <validation/validation_dsl.hxx>

UpdateGuardModeDto UpdateGuardModeDto::fromJson(const Json::Value& json)
{
  UpdateGuardModeDto dto;
  dto.mode = json.get("mode", "").asString();
  const bool hasEnvironment =
      json.isMember("environmentId") && !json["environmentId"].isNull();
  if (hasEnvironment && json["environmentId"].isInt64())
    dto.environmentId = json["environmentId"].asInt64();

  START_VALIDATION(UpdateGuardModeDto, dto)
  IS_NOT_EMPTY(mode)
  IS_IN(mode, "home", "away", "night", "armed")
  CUSTOM_LAMBDA(environmentId,
                [hasEnvironment](const UpdateGuardModeDto& value)
                    -> std::optional<std::string> {
                  if (!hasEnvironment)
                    return std::nullopt;
                  if (!value.environmentId || *value.environmentId <= 0)
                    return "must be a positive integer";
                  return std::nullopt;
                })
  END_VALIDATION()
  return dto;
}
