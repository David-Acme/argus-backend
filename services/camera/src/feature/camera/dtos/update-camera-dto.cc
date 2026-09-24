#include "update-camera-dto.hxx"

UpdateCameraDto UpdateCameraDto::fromJson(const Json::Value& json)
{
  UpdateCameraDto dto;
  if (json.isMember("name") && json["name"].isString())
    dto.name = json["name"].asString();
  if (json.isMember("manufacturer") && json["manufacturer"].isString())
    dto.manufacturer = json["manufacturer"].asString();
  if (json.isMember("model") && json["model"].isString())
    dto.model = json["model"].asString();
  if (json.isMember("ip") && json["ip"].isString())
    dto.ip = json["ip"].asString();
  if (json.isMember("port") && json["port"].isInt())
    dto.port = json["port"].asInt();
  if (json.isMember("username") && json["username"].isString())
    dto.username = json["username"].asString();
  if (json.isMember("password") && json["password"].isString())
    dto.password = json["password"].asString();
  if (json.isMember("cloudUsername") && json["cloudUsername"].isString())
    dto.cloudUsername = json["cloudUsername"].asString();
  if (json.isMember("cloudPassword") && json["cloudPassword"].isString())
    dto.cloudPassword = json["cloudPassword"].asString();
  if (json.isMember("driver") && json["driver"].isString())
    dto.driver = json["driver"].asString();
  if (json.isMember("icon") && json["icon"].isString())
    dto.icon = json["icon"].asString();
  if (json.isMember("recordMode") && json["recordMode"].isString())
    dto.recordMode = json["recordMode"].asString();
  if (json.isMember("retentionDays") && json["retentionDays"].isInt64())
    dto.retentionDays = json["retentionDays"].asInt64();
  if (json.isMember("isEnabled") && json["isEnabled"].isBool())
    dto.isEnabled = json["isEnabled"].asBool();

  START_VALIDATION(UpdateCameraDto, dto)
  IS_NOT_EMPTY_OPTIONAL(name)
  MAX_LENGTH_OPTIONAL(name, 120)
  IS_NOT_EMPTY_OPTIONAL(ip)
  MAX_LENGTH_OPTIONAL(ip, 64)
  MAX_LENGTH_OPTIONAL(icon, 40)
  CUSTOM_LAMBDA(driver, [](const UpdateCameraDto& d) -> std::optional<std::string> {
    if (!d.driver)
      return std::nullopt;
    if (*d.driver == "tapo" || *d.driver == "onvif" || *d.driver == "rtsp")
      return std::nullopt;
    return "must be one of: tapo, onvif, rtsp";
  })
  END_VALIDATION()
  return dto;
}
