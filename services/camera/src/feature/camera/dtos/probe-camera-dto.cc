#include "probe-camera-dto.hxx"

#include <feature/camera/dtos/camera-address-rules.hxx>
#include <validation/validation_dsl.hxx>

ProbeCameraDto ProbeCameraDto::fromJson(const Json::Value& json)
{
  ProbeCameraDto dto;
  dto.driver = json.get("driver", "tapo").asString();
  dto.ip = json.get("ip", "").asString();
  if (json.isMember("port") && json["port"].isInt())
    dto.port = json["port"].asInt();
  dto.username = json.get("username", "").asString();
  dto.password = json.get("password", "").asString();
  dto.cloudUsername = json.get("cloudUsername", "").asString();
  dto.cloudPassword = json.get("cloudPassword", "").asString();
  dto.streamPath = json.get("streamPath", "").asString();
  dto.subStreamPath = json.get("subStreamPath", "").asString();
  if (json.isMember("cameraId") && json["cameraId"].isInt64())
    dto.cameraId = json["cameraId"].asInt64();

  START_VALIDATION(ProbeCameraDto, dto)
  IS_IN(driver, "tapo", "onvif", "rtsp")
  IS_NOT_EMPTY(ip)
  MAX_LENGTH(ip, 64)
  CUSTOM_LAMBDA(ip, [](const ProbeCameraDto& d) -> std::optional<std::string> {
    return camera_address_rules::addressError(d.ip);
  })
  BETWEEN(port, 1, 65535)
  MAX_LENGTH(username, 80)
  MAX_LENGTH(password, camera_address_rules::kMaxSecretLength)
  MAX_LENGTH(cloudUsername, 120)
  MAX_LENGTH(cloudPassword, camera_address_rules::kMaxSecretLength)
  CUSTOM_LAMBDA(streamPath, [](const ProbeCameraDto& d) {
    return camera_address_rules::pathError(d.streamPath);
  })
  CUSTOM_LAMBDA(subStreamPath, [](const ProbeCameraDto& d) {
    return camera_address_rules::pathError(d.subStreamPath);
  })
  CUSTOM_LAMBDA(cameraId, [](const ProbeCameraDto& d) -> std::optional<std::string> {
    if (!d.cameraId || *d.cameraId > 0)
      return std::nullopt;
    return "must be a positive camera id";
  })
  END_VALIDATION()
  return dto;
}
