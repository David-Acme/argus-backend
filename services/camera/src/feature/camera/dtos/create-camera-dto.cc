#include "create-camera-dto.hxx"

#include <feature/camera/dtos/camera-address-rules.hxx>

CreateCameraDto CreateCameraDto::fromJson(const Json::Value& json)
{
  CreateCameraDto dto;
  dto.name = json.get("name", "").asString();
  dto.manufacturer = json.get("manufacturer", "").asString();
  dto.model = json.get("model", "").asString();
  dto.ip = json.get("ip", "").asString();
  if (json.isMember("port") && json["port"].isInt())
    dto.port = json["port"].asInt();
  dto.username = json.get("username", "").asString();
  dto.password = json.get("password", "").asString();
  dto.cloudUsername = json.get("cloudUsername", "").asString();
  dto.cloudPassword = json.get("cloudPassword", "").asString();
  dto.driver = json.get("driver", "tapo").asString();
  dto.icon = json.get("icon", "video").asString();
  dto.recordMode = json.get("recordMode", "events").asString();
  if (json.isMember("retentionDays") && json["retentionDays"].isInt64())
    dto.retentionDays = json["retentionDays"].asInt64();
  if (json.isMember("streamPath") && json["streamPath"].isString())
    dto.streamPath = json["streamPath"].asString();
  if (json.isMember("subStreamPath") && json["subStreamPath"].isString())
    dto.subStreamPath = json["subStreamPath"].asString();
  if (json.isMember("catalogId") && json["catalogId"].isString())
    dto.catalogId = json["catalogId"].asString();
  if (json.isMember("retentionIncident") && json["retentionIncident"].isBool())
    dto.retentionIncident = json["retentionIncident"].asBool();

  START_VALIDATION(CreateCameraDto, dto)
  IS_NOT_EMPTY(name)
  MAX_LENGTH(name, 120)
  IS_NOT_EMPTY(ip)
  MAX_LENGTH(ip, 64)
  CUSTOM_LAMBDA(ip, [](const CreateCameraDto& d) -> std::optional<std::string> {
    return camera_address_rules::addressError(d.ip);
  })
  BETWEEN(port, 1, 65535)
  IS_IN(recordMode, "events", "continuous")
  IS_IN(driver, "tapo", "onvif", "rtsp")
  MAX_LENGTH(cloudUsername, 120)
  IS_NOT_EMPTY(icon)
  MAX_LENGTH(icon, 40)
  MAX_LENGTH(manufacturer, 80)
  MAX_LENGTH(model, 80)
  MAX_LENGTH(username, 80)
  MAX_LENGTH(password, camera_address_rules::kMaxSecretLength)
  MAX_LENGTH(cloudPassword, camera_address_rules::kMaxSecretLength)
  CUSTOM_LAMBDA(retentionDays, [](const CreateCameraDto& d) {
    return camera_address_rules::retentionError(
        {.days = d.retentionDays, .incident = d.retentionIncident.value_or(false)});
  })
  CUSTOM_LAMBDA(streamPath, [](const CreateCameraDto& d) {
    return camera_address_rules::pathError(d.streamPath);
  })
  CUSTOM_LAMBDA(subStreamPath, [](const CreateCameraDto& d) {
    return camera_address_rules::pathError(d.subStreamPath);
  })
  CUSTOM_LAMBDA(catalogId, [](const CreateCameraDto& d) {
    return camera_address_rules::catalogError(d.catalogId);
  })
  END_VALIDATION()
  return dto;
}
