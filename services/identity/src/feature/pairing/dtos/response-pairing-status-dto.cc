#include "response-pairing-status-dto.hxx"

Json::Value ResponsePairingStatusDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["paired"] = paired;
  json["hasOwner"] = hasOwner;
  return json;
}
