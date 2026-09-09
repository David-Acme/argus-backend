#include "create-zone-dto.hxx"

#include <shared/utils/geometry/normalized-polygon.hxx>

CreateZoneDto CreateZoneDto::fromJson(const Json::Value& json)
{
  CreateZoneDto dto;
  if (json.isMember("cameraId") && json["cameraId"].isInt64())
    dto.cameraId = json["cameraId"].asInt64();
  dto.name = json.get("name", "").asString();
  dto.zoneType = json.get("zoneType", "monitor").asString();
  dto.color = json.get("color", "#FF0000").asString();
  if (json.isMember("isEnabled") && json["isEnabled"].isBool())
    dto.isEnabled = json["isEnabled"].asBool();

  const auto points =
      geometry::serializeNormalizedPolygon(json["points"]);
  dto.points = points.value_or("");

  START_VALIDATION(CreateZoneDto, dto)
  IS_POSITIVE(cameraId)
  IS_NOT_EMPTY(name)
  MAX_LENGTH(name, 120)
  IS_IN(zoneType, "monitor", "alert", "exclude")
  MATCHES_REGEX(color, "^#[0-9A-Fa-f]{6}$", "must be a #RRGGBB hex color")
  CUSTOM_LAMBDA(points,
                [](const CreateZoneDto& d) -> std::optional<std::string> {
                  if (d.points.empty())
                    return "must be 3 to 64 points with x and y in [0, 1]";
                  return std::nullopt;
                })
  END_VALIDATION()
  return dto;
}
