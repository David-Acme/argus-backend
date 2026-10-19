#include "declare-maintenance-dto.hxx"

#include <validation/validation_dsl.hxx>

namespace
{
constexpr int64_t kMaxMinutes = 120;
constexpr size_t kMaxCameras = 64;
}

DeclareMaintenanceDto DeclareMaintenanceDto::fromJson(const Json::Value& json)
{
  DeclareMaintenanceDto dto;
  if (json.isMember("cameraIds")) {
    dto.idsTyped = json["cameraIds"].isArray();
    if (dto.idsTyped)
      for (const auto& id : json["cameraIds"]) {
        if (!id.isInt64() || id.asInt64() <= 0) {
          dto.idsTyped = false;
          break;
        }
        dto.cameraIds.push_back(id.asInt64());
      }
  }
  if (json.isMember("environmentId") && json["environmentId"].isInt64())
    dto.environmentId = json["environmentId"].asInt64();
  if (json.isMember("minutes") && json["minutes"].isInt64())
    dto.minutes = json["minutes"].asInt64();

  START_VALIDATION(DeclareMaintenanceDto, dto)
  IS_NON_NEGATIVE(environmentId)
  CUSTOM_LAMBDA(cameraIds, [](const DeclareMaintenanceDto& value) -> std::optional<std::string> {
    if (!value.idsTyped)
      return "must be a list of camera ids";
    if (value.cameraIds.size() > kMaxCameras)
      return "names at most 64 cameras";
    if (value.cameraIds.empty() && value.environmentId <= 0)
      return "name cameras or an environment";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(minutes, [](const DeclareMaintenanceDto& value) -> std::optional<std::string> {
    if (value.minutes < 0 || value.minutes > kMaxMinutes)
      return "must be between 1 and 120";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
