#include "camera-settings-dto.hxx"

CameraSettingsDto CameraSettingsDto::fromJson(const Json::Value& json)
{
  CameraSettingsDto dto;
  if (json.isMember("privacy") && json["privacy"].isBool())
    dto.privacy = json["privacy"].asBool();
  if (json.isMember("led") && json["led"].isBool())
    dto.led = json["led"].asBool();
  if (json.isMember("dayNight") && json["dayNight"].isString())
    dto.dayNight = json["dayNight"].asString();
  if (json.isMember("motion") && json["motion"].isBool())
    dto.motion = json["motion"].asBool();
  if (json.isMember("motionSensitivity") && json["motionSensitivity"].isInt())
    dto.motionSensitivity = json["motionSensitivity"].asInt();
  if (json.isMember("autoTrack") && json["autoTrack"].isBool())
    dto.autoTrack = json["autoTrack"].asBool();
  if (json.isMember("alarm") && json["alarm"].isBool())
    dto.alarm = json["alarm"].asBool();
  if (json.isMember("alarmVolume") && json["alarmVolume"].isInt())
    dto.alarmVolume = json["alarmVolume"].asInt();

  START_VALIDATION(CameraSettingsDto, dto)
  CUSTOM_LAMBDA(dayNight, [](const CameraSettingsDto& d) -> std::optional<std::string> {
    if (!d.dayNight)
      return std::nullopt;
    if (*d.dayNight == "auto" || *d.dayNight == "day" || *d.dayNight == "night")
      return std::nullopt;
    return "must be one of: auto, day, night";
  })
  CUSTOM_LAMBDA(motionSensitivity,
                [](const CameraSettingsDto& d) -> std::optional<std::string> {
                  if (!d.motionSensitivity)
                    return std::nullopt;
                  return *d.motionSensitivity >= 1 && *d.motionSensitivity <= 100
                             ? std::nullopt
                             : std::optional<std::string>("must be between 1 and 100");
                })
  CUSTOM_LAMBDA(alarmVolume, [](const CameraSettingsDto& d) -> std::optional<std::string> {
    if (!d.alarmVolume)
      return std::nullopt;
    return *d.alarmVolume >= 1 && *d.alarmVolume <= 100
               ? std::nullopt
               : std::optional<std::string>("must be between 1 and 100");
  })
  END_VALIDATION()
  return dto;
}
