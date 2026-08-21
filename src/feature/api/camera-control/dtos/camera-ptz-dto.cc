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
    if (d.angle)
      return std::nullopt;
    if (d.x && d.y)
      return std::nullopt;
    return "send an angle for a step, or x and y for an absolute move";
  })
  END_VALIDATION()
  return dto;
}
