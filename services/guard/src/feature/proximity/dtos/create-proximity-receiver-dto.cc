#include "create-proximity-receiver-dto.hxx"

#include <validation/validation_dsl.hxx>

CreateProximityReceiverDto CreateProximityReceiverDto::fromJson(const Json::Value& json)
{
  CreateProximityReceiverDto dto;
  dto.name = json.get("name", "").asString();
  dto.room = json.get("room", "").asString();
  dto.kind = json.get("kind", "esphome").asString();
  dto.host = json.get("host", "").asString();
  dto.port = json["port"].isIntegral() ? json["port"].asInt64() : 0;

  START_VALIDATION(CreateProximityReceiverDto, dto)
  IS_NOT_EMPTY(name)
  MAX_LENGTH(name, 64)
  MAX_LENGTH(room, 64)
  IS_IN(kind, "esphome")
  IS_NOT_EMPTY(host)
  MAX_LENGTH(host, 253)
  MATCHES_REGEX(host, "^[A-Za-z0-9.:\\-\\[\\]]+$", "must be a host name or an IP address")
  BETWEEN(port, 1, 65535)
  END_VALIDATION()
  return dto;
}
