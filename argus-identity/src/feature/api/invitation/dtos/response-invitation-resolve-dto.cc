#include "response-invitation-resolve-dto.hxx"

Json::Value ResponseInvitationResolveDto::toJson() const
{
  Json::Value value(Json::objectValue);
  value["role"] = userRoleToString(role);
  value["expiresAt"] = Json::Int64(expiresAt);
  value["instanceId"] = instanceId;
  value["caFingerprint"] = caFingerprint;
  value["serverFingerprint"] = serverFingerprint;
  value["caPem"] = caPem;
  value["scheme"] = scheme;
  value["port"] = port;
  return value;
}
