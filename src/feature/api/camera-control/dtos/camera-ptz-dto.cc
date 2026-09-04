#include "camera-ptz-dto.hxx"

CameraPtzDto CameraPtzDto::fromJson(const Json::Value& json)
{
  CameraPtzDto dto;
  if (json.isMember("x") && json["x"].isInt64())
    dto.x = json["x"].asInt64();
  if (json.isMember("y") && json["y"].isInt64())
    dto.y = json["y"].asInt64();
  if (json.isMember("angle") && json["angle"].isInt64())
    dto.angle = json["angle"].asInt64();

  START_VALIDATION(CameraPtzDto, dto)
  CUSTOM_LAMBDA(angle, [](const CameraPtzDto& d) -> std::optional<std::string> {
    if (!d.angle || (*d.angle >= 0 && *d.angle < 360))
      return std::nullopt;
    return "direction must be between 0 and 359";
  })
  CUSTOM_LAMBDA(x, [](const CameraPtzDto& d) -> std::optional<std::string> {
    if (d.angle || (d.x && d.y))
      return std::nullopt;
    return "send a protocol direction, or x and y for an absolute move";
  })
  END_VALIDATION()
  return dto;
}
