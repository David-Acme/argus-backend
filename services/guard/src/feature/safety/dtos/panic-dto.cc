#include "panic-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <string>

PanicDto PanicDto::fromJson(const Json::Value& json)
{
  PanicDto dto;
  const bool hasEnvironment =
      json.isObject() && json.isMember("environmentId") && !json["environmentId"].isNull();
  if (hasEnvironment && json["environmentId"].isInt64())
    dto.environmentId = json["environmentId"].asInt64();

  START_VALIDATION(PanicDto, dto)
  CUSTOM_LAMBDA(environmentId,
                [hasEnvironment](const PanicDto& value) -> std::optional<std::string> {
                  if (!hasEnvironment)
                    return std::nullopt;
                  if (!value.environmentId || *value.environmentId <= 0)
                    return "must be a positive integer";
                  return std::nullopt;
                })
  END_VALIDATION()
  return dto;
}
