#include "create-environment-dto.hxx"

#include <validation/validation_dsl.hxx>

CreateEnvironmentDto CreateEnvironmentDto::fromJson(const Json::Value& json)
{
  CreateEnvironmentDto dto;
  if (json.isMember("mode") && json["mode"].isString())
    dto.mode = json["mode"].asString();
  START_VALIDATION(CreateEnvironmentDto, dto)
  IS_IN_OPTIONAL(mode, "home", "away", "night", "armed")
  CUSTOM_LAMBDA(mode,
                [&json](const CreateEnvironmentDto&)
                    -> std::optional<std::string> {
                  if (json.isMember("mode") && !json["mode"].isString())
                    return "wrong type for mode";
                  return std::nullopt;
                })
  CUSTOM_LAMBDA(name,
                [&json](const CreateEnvironmentDto&)
                    -> std::optional<std::string> {
                  if (!json.isMember("name"))
                    return "is required";
                  return std::nullopt;
                })
  CUSTOM_LAMBDA(kind,
                [&json](const CreateEnvironmentDto&)
                    -> std::optional<std::string> {
                  if (!json.isMember("kind"))
                    return "is required";
                  return std::nullopt;
                })
  END_VALIDATION()
  dto.fields = UpdateEnvironmentDto::fromJson(json);
  return dto;
}
