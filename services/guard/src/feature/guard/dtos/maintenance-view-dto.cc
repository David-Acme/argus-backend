#include "maintenance-view-dto.hxx"

#include <validation/validation_dsl.hxx>

MaintenanceViewDto MaintenanceViewDto::fromJson(const Json::Value& json)
{
  MaintenanceViewDto dto;
  if (json.isMember("cameraId") && json["cameraId"].isInt64())
    dto.cameraId = json["cameraId"].asInt64();
  dto.typed = json["accept"].isBool();
  dto.accept = dto.typed && json["accept"].asBool();

  START_VALIDATION(MaintenanceViewDto, dto)
  IS_POSITIVE(cameraId)
  CUSTOM_LAMBDA(accept, [](const MaintenanceViewDto& value) -> std::optional<std::string> {
    if (value.typed)
      return std::nullopt;
    return "must be true or false";
  })
  END_VALIDATION()
  return dto;
}
