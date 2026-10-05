#include "update-visitor-settings-dto.hxx"

#include <validation/validation_dsl.hxx>

UpdateVisitorSettingsDto UpdateVisitorSettingsDto::fromJson(const Json::Value& json)
{
  UpdateVisitorSettingsDto dto;
  if (json["unnamedRetentionDays"].isIntegral())
    dto.unnamedRetentionDays = json["unnamedRetentionDays"].asInt64();
  START_VALIDATION(UpdateVisitorSettingsDto, dto)
  BETWEEN(unnamedRetentionDays, 1, 60)
  END_VALIDATION()
  return dto;
}
