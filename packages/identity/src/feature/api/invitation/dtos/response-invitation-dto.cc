#include "response-invitation-dto.hxx"

Json::Value ResponseInvitationDto::toJson() const
{
  Json::Value value(Json::objectValue);
  value["id"] = invitation.id;
  value["role"] = userRoleToString(invitation.role);
  value["maxRedemptions"] = invitation.maxRedemptions;
  value["redemptionCount"] = invitation.redemptionCount;
  value["expiresAt"] = Json::Int64(invitation.expiresAt);
  value["createdBy"] = invitation.createdBy;
  value["revokedAt"] = invitation.revokedAt
                           ? Json::Value(Json::Int64(*invitation.revokedAt))
                           : Json::Value();
  value["createdAt"] = Json::Int64(invitation.createdAt);
  if (!token.empty())
    value["token"] = token;
  return value;
}
