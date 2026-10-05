#include "update-safety-dto.hxx"

#include <feature/safety/services/pin-hash.hxx>

#include <validation/validation_dsl.hxx>

UpdateSafetyDto UpdateSafetyDto::fromJson(const Json::Value& json)
{
  UpdateSafetyDto dto;
  const bool present = json.isMember("duressEnabled") && json["duressEnabled"].isBool();
  if (present)
    dto.duressEnabled = json["duressEnabled"].asBool();
  const bool hasCurrent = json.isMember("currentPin") && !json["currentPin"].isNull();
  if (hasCurrent && json["currentPin"].isString())
    dto.currentPin = json["currentPin"].asString();

  START_VALIDATION(UpdateSafetyDto, dto)
  CUSTOM_LAMBDA(duressEnabled, [present](const UpdateSafetyDto&) -> std::optional<std::string> {
    if (!present)
      return "must be a boolean";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(currentPin, [hasCurrent](const UpdateSafetyDto& value) -> std::optional<std::string> {
    if (hasCurrent && (!value.currentPin || !pin_hash::wellFormedPin(*value.currentPin)))
      return "must be 4 to 8 digits";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
