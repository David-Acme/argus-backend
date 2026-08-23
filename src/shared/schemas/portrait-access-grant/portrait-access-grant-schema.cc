#include "portrait-access-grant-schema.hxx"

#include <drogon/orm/Field.h>

PortraitAccessGrantSchema::PortraitAccessGrantSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  if (!row["request_id"].isNull())
    requestId = static_cast<int64_t>(row["request_id"].as<long long>());
  portraitUserId = static_cast<int64_t>(row["portrait_user_id"].as<long long>());
  granteeUserId = static_cast<int64_t>(row["grantee_user_id"].as<long long>());
  grantedBy = static_cast<int64_t>(row["granted_by"].as<long long>());
  scope = portraitAccessGrantScopeFromString(row["scope"].as<std::string>());
  if (!row["expires_at"].isNull())
    expiresAt = static_cast<int64_t>(row["expires_at"].as<long long>());
  if (!row["revoked_at"].isNull())
    revokedAt = static_cast<int64_t>(row["revoked_at"].as<long long>());
  if (!row["revoked_by"].isNull())
    revokedBy = static_cast<int64_t>(row["revoked_by"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
}
