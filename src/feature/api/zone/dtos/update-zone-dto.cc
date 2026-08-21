#include "update-zone-dto.hxx"

#include <regex>
#include <shared/utils/geometry/normalized-polygon.hxx>

UpdateZoneDto UpdateZoneDto::fromJson(const Json::Value& json)
{
  UpdateZoneDto dto;
  if (json.isMember("name") && json["name"].isString())
    dto.name = json["name"].asString();
  if (json.isMember("zoneType") && json["zoneType"].isString())
    dto.zoneType = json["zoneType"].asString();
  if (json.isMember("color") && json["color"].isString())
    dto.color = json["color"].asString();
  if (json.isMember("isEnabled") && json["isEnabled"].isBool())
    dto.isEnabled = json["isEnabled"].asBool();

  // An empty string marks "sent but invalid" so the rule below can reject it;
  // an absent key stays nullopt and leaves the column untouched.
  if (json.isMember("points"))
    dto.points =
        geometry::serializeNormalizedPolygon(json["points"]).value_or("");

  START_VALIDATION(UpdateZoneDto, dto)
  IS_NOT_EMPTY_OPTIONAL(name)
  MAX_LENGTH_OPTIONAL(name, 120)
  CUSTOM_LAMBDA(zoneType,
                [](const UpdateZoneDto& d) -> std::optional<std::string> {
                  if (!d.zoneType)
                    return std::nullopt;
                  if (*d.zoneType == "monitor" || *d.zoneType == "alert" ||
                      *d.zoneType == "exclude")
                    return std::nullopt;
                  return "must be one of: monitor, alert, exclude";
                })
  CUSTOM_LAMBDA(color,
                [](const UpdateZoneDto& d) -> std::optional<std::string> {
                  if (!d.color)
                    return std::nullopt;
                  static const std::regex kHex("^#[0-9A-Fa-f]{6}$");
                  if (std::regex_match(*d.color, kHex))
                    return std::nullopt;
                  return "must be a #RRGGBB hex color";
                })
  CUSTOM_LAMBDA(points,
                [](const UpdateZoneDto& d) -> std::optional<std::string> {
                  if (d.points && d.points->empty())
                    return "must be 3 to 64 points with x and y in [0, 1]";
                  return std::nullopt;
                })
  END_VALIDATION()
  return dto;
}
