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
  if (json.isMember("stop") && json["stop"].isBool())
    dto.stop = json["stop"].asBool();

  START_VALIDATION(CameraPtzDto, dto)
  CUSTOM_LAMBDA(angle, [](const CameraPtzDto& d) -> std::optional<std::string> {
    if (!d.angle || (*d.angle >= 0 && *d.angle < 360))
      return std::nullopt;
    return "direction must be between 0 and 359";
  })
  CUSTOM_LAMBDA(x, [](const CameraPtzDto& d) -> std::optional<std::string> {
    const int commands = (d.stop ? 1 : 0) + (d.angle ? 1 : 0) + (d.x || d.y ? 1 : 0);
    if (commands != 1)
      return "send x and y for a step, angle for a continuous move, or stop";
    if (d.x || d.y) {
      if (!d.x || !d.y)
        return "a step needs both x and y";
      if (*d.x < -kMaxStepDegrees || *d.x > kMaxStepDegrees || *d.y < -kMaxStepDegrees ||
          *d.y > kMaxStepDegrees)
        return "a step is at most 180 degrees each way";
    }
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
