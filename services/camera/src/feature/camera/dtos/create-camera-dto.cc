#include "create-camera-dto.hxx"

#include <arpa/inet.h>

namespace
{
bool isLiteralAddress(const std::string& value)
{
  in_addr v4{};
  in6_addr v6{};
  return ::inet_pton(AF_INET, value.c_str(), &v4) == 1 ||
         ::inet_pton(AF_INET6, value.c_str(), &v6) == 1;
}
}

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

  START_VALIDATION(CreateCameraDto, dto)
  IS_NOT_EMPTY(name)
  MAX_LENGTH(name, 120)
  IS_NOT_EMPTY(ip)
  MAX_LENGTH(ip, 64)
  CUSTOM_LAMBDA(ip, [](const CreateCameraDto& d) -> std::optional<std::string> {
    if (isLiteralAddress(d.ip))
      return std::nullopt;
    return "must be an IPv4 or IPv6 address";
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
  END_VALIDATION()
  return dto;
}
