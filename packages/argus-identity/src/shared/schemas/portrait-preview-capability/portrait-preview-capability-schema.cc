#include "portrait-preview-capability-schema.hxx"

#include <drogon/orm/Field.h>

PortraitPreviewCapabilitySchema::PortraitPreviewCapabilitySchema(
    const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  tokenHash = row["token_hash"].as<std::string>();
  portraitUserId = static_cast<int64_t>(row["portrait_user_id"].as<long long>());
  requesterUserId = static_cast<int64_t>(row["requester_user_id"].as<long long>());
  expiresAt = static_cast<int64_t>(row["expires_at"].as<long long>());
  if (!row["consumed_at"].isNull())
    consumedAt = static_cast<int64_t>(row["consumed_at"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
}
