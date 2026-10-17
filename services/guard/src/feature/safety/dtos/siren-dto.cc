#include "siren-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <optional>

SirenDto SirenDto::fromJson(const Json::Value& json)
{
  SirenDto dto;
  const bool hasCamera =
      json.isMember("cameraId") && json["cameraId"].isInt64();
  if (hasCamera)
    dto.cameraId = json["cameraId"].asInt64();
  const bool hasEnabled = json.isMember("enabled") && json["enabled"].isBool();
  if (hasEnabled)
    dto.enabled = json["enabled"].asBool();

  START_VALIDATION(SirenDto, dto)
  CUSTOM_LAMBDA(cameraId, [hasCamera](const SirenDto& value) -> std::optional<std::string> {
    if (!hasCamera || value.cameraId <= 0)
      return "must be a positive integer";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(enabled, [hasEnabled](const SirenDto&) -> std::optional<std::string> {
    if (!hasEnabled)
      return "must be a boolean";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
