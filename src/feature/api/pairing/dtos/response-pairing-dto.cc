#include "response-pairing-dto.hxx"

Json::Value ResponsePairingDto::toJson() const
{
  Json::Value value(Json::objectValue);
  value["instanceId"] = instanceId;
  value["caFingerprint"] = caFingerprint;
  value["serverFingerprint"] = serverFingerprint;
  value["caPem"] = caPem;
  value["scheme"] = scheme;
  value["port"] = port;
  return value;
}
