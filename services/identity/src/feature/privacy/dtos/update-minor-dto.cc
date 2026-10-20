#include "update-minor-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <string>

namespace
{
std::optional<bool> boolOf(const Json::Value& json, const char* key)
{
  if (json.isMember(key) && json[key].isBool())
    return json[key].asBool();
  return std::nullopt;
}
}

UpdateMinorDto UpdateMinorDto::fromJson(const Json::Value& json, int64_t userId)
{
  UpdateMinorDto dto;
  dto.userId = userId;
  dto.minor = boolOf(json, "minor");
  dto.guardianConsent = boolOf(json, "guardianConsent");

  START_VALIDATION(UpdateMinorDto, dto)
  IS_POSITIVE(userId)
  CUSTOM_LAMBDA(minor, [](const UpdateMinorDto& value) -> std::optional<std::string> {
    if (!value.minor)
      return "minor must be a boolean";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(guardianConsent, [](const UpdateMinorDto& value) -> std::optional<std::string> {
    if (value.guardianConsent.value_or(false) && !value.minor.value_or(false))
      return "guardianConsent applies only to a minor";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
