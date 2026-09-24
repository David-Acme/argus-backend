#include "user-invitation-schema.hxx"

#include <drogon/orm/Field.h>

UserInvitationSchema::UserInvitationSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  tokenHash = row["token_hash"].as<std::string>();
  role = userRoleFromString(row["role"].as<std::string>());
  maxRedemptions = row["max_redemptions"].as<int>();
  redemptionCount = row["redemption_count"].as<int>();
  expiresAt = static_cast<int64_t>(row["expires_at"].as<long long>());
  createdBy = static_cast<int64_t>(row["created_by"].as<long long>());
  if (!row["revoked_at"].isNull())
    revokedAt = static_cast<int64_t>(row["revoked_at"].as<long long>());
  if (!row["revoked_by"].isNull())
    revokedBy = static_cast<int64_t>(row["revoked_by"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
}

Json::Value UserInvitationSchema::toJson() const
{
  Json::Value value(Json::objectValue);
  value["id"] = id;
  value["role"] = userRoleToString(role);
  value["maxRedemptions"] = maxRedemptions;
  value["redemptionCount"] = redemptionCount;
  value["expiresAt"] = Json::Int64(expiresAt);
  value["createdBy"] = createdBy;
  value["revokedAt"] = revokedAt ? Json::Value(Json::Int64(*revokedAt))
                               : Json::Value();
  value["createdAt"] = Json::Int64(createdAt);
  value["updatedAt"] = updatedAt ? Json::Value(Json::Int64(*updatedAt))
                                : Json::Value();
  value["syncAt"] = Json::Int64(updatedAt.value_or(createdAt));
  return value;
}
