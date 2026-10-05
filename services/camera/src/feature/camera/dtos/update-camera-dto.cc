#include "update-camera-dto.hxx"

#include <feature/camera/dtos/camera-address-rules.hxx>

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
  if (json.isMember("streamPath") && json["streamPath"].isString())
    dto.streamPath = json["streamPath"].asString();
  if (json.isMember("subStreamPath") && json["subStreamPath"].isString())
    dto.subStreamPath = json["subStreamPath"].asString();
  if (json.isMember("catalogId") && json["catalogId"].isString())
    dto.catalogId = json["catalogId"].asString();
  if (json.isMember("retentionIncident") && json["retentionIncident"].isBool())
    dto.retentionIncident = json["retentionIncident"].asBool();

  START_VALIDATION(UpdateCameraDto, dto)
  IS_NOT_EMPTY_OPTIONAL(name)
  MAX_LENGTH_OPTIONAL(name, 120)
  IS_NOT_EMPTY_OPTIONAL(ip)
  MAX_LENGTH_OPTIONAL(ip, 64)
  CUSTOM_LAMBDA(ip, [](const UpdateCameraDto& d) -> std::optional<std::string> {
    if (!d.ip)
      return std::nullopt;
    return camera_address_rules::addressError(*d.ip);
  })
  CUSTOM_LAMBDA(port, [](const UpdateCameraDto& d) -> std::optional<std::string> {
    if (!d.port || (*d.port >= 1 && *d.port <= 65535))
      return std::nullopt;
    return "must be between 1 and 65535";
  })
  IS_IN_OPTIONAL(recordMode, "events", "continuous")
  IS_NOT_EMPTY_OPTIONAL(icon)
  MAX_LENGTH_OPTIONAL(icon, 40)
  MAX_LENGTH_OPTIONAL(manufacturer, 80)
  MAX_LENGTH_OPTIONAL(model, 80)
  MAX_LENGTH_OPTIONAL(username, 80)
  MAX_LENGTH_OPTIONAL(cloudUsername, 120)
  MAX_LENGTH_OPTIONAL(password, camera_address_rules::kMaxSecretLength)
  MAX_LENGTH_OPTIONAL(cloudPassword, camera_address_rules::kMaxSecretLength)
  CUSTOM_LAMBDA(retentionDays, [](const UpdateCameraDto& d) {
    return camera_address_rules::retentionError(
        {.days = d.retentionDays, .incident = d.retentionIncident.value_or(true)});
  })
  CUSTOM_LAMBDA(streamPath, [](const UpdateCameraDto& d) {
    return camera_address_rules::pathError(d.streamPath);
  })
  CUSTOM_LAMBDA(subStreamPath, [](const UpdateCameraDto& d) {
    return camera_address_rules::pathError(d.subStreamPath);
  })
  CUSTOM_LAMBDA(catalogId, [](const UpdateCameraDto& d) {
    return camera_address_rules::catalogError(d.catalogId);
  })
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
