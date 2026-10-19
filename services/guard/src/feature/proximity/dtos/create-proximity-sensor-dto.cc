#include "create-proximity-sensor-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <string>

CreateProximitySensorDto CreateProximitySensorDto::fromJson(const Json::Value& json)
{
  CreateProximitySensorDto dto;
  dto.name = json.get("name", "").asString();
  dto.room = json.get("room", "").asString();
  dto.model = json.get("model", "").asString();
  dto.source = json.get("source", "").asString();
  dto.host = json.get("host", "").asString();
  dto.port = json["port"].isIntegral() ? json["port"].asInt64() : 0;
  dto.entityId = json.get("entityId", "").asString();
  const Json::Value& camera = json["cameraId"];
  const bool cameraGiven = !camera.isNull();
  if (camera.isIntegral())
    dto.cameraId = camera.asInt64();
  else if (camera.isString() && !camera.asString().empty() &&
           camera.asString().find_first_not_of("0123456789") == std::string::npos && camera.asString().size() < 19)
    dto.cameraId = std::stoll(camera.asString());
  const Json::Value& pan = json["panOffsetDeg"];
  const bool panGiven = !pan.isNull();
  if (pan.isNumeric())
    dto.panOffsetDeg = pan.asDouble();

  START_VALIDATION(CreateProximitySensorDto, dto)
  IS_NOT_EMPTY(name)
  MAX_LENGTH(name, 64)
  MAX_LENGTH(room, 64)
  IS_IN(model, "ld2410", "ld2450", "fp2")
  IS_IN(source, "esphome", "home_assistant")
  IS_NOT_EMPTY(host)
  MAX_LENGTH(host, 253)
  MATCHES_REGEX(host, "^[A-Za-z0-9.:\\-\\[\\]]+$", "must be a host name or an IP address")
  BETWEEN(port, 1, 65535)
  IS_NOT_EMPTY(entityId)
  MAX_LENGTH(entityId, 128)
  MATCHES_REGEX(entityId, "^[A-Za-z0-9_.\\-]+$", "must be an entity id")
  CUSTOM_LAMBDA(cameraId, [cameraGiven](const CreateProximitySensorDto& value) -> std::optional<std::string> {
    if (cameraGiven && (!value.cameraId || *value.cameraId <= 0))
      return "must be a camera id";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(panOffsetDeg, [panGiven](const CreateProximitySensorDto& value) -> std::optional<std::string> {
    if (panGiven && (!value.panOffsetDeg || *value.panOffsetDeg < -180.0 || *value.panOffsetDeg > 180.0))
      return "must be between -180 and 180";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
