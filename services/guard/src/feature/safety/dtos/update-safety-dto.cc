#include "update-safety-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <string>

UpdateSafetyDto UpdateSafetyDto::fromJson(const Json::Value& json)
{
  UpdateSafetyDto dto;
  const bool present = json.isMember("duressEnabled") && json["duressEnabled"].isBool();
  if (present)
    dto.duressEnabled = json["duressEnabled"].asBool();

  START_VALIDATION(UpdateSafetyDto, dto)
  CUSTOM_LAMBDA(duressEnabled, [present](const UpdateSafetyDto&) -> std::optional<std::string> {
    if (!present)
      return "must be a boolean";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
