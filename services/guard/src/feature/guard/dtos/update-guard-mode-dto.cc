#include "update-guard-mode-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <algorithm>

UpdateGuardModeDto UpdateGuardModeDto::fromJson(const Json::Value& json)
{
  UpdateGuardModeDto dto;
  dto.mode = json.get("mode", "").asString();
  const bool hasEnvironment =
      json.isMember("environmentId") && !json["environmentId"].isNull();
  if (hasEnvironment && json["environmentId"].isInt64())
    dto.environmentId = json["environmentId"].asInt64();
  const bool hasPin = json.isMember("pin") && !json["pin"].isNull();
  if (hasPin && json["pin"].isString())
    dto.pin = json["pin"].asString();

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
  CUSTOM_LAMBDA(pin, [hasPin](const UpdateGuardModeDto& value) -> std::optional<std::string> {
    if (!hasPin)
      return std::nullopt;
    if (!value.pin || value.pin->size() < 4 || value.pin->size() > 8 ||
        !std::ranges::all_of(*value.pin, [](char c) { return c >= '0' && c <= '9'; }))
      return "must be 4 to 8 digits";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
