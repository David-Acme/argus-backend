#include "portrait-access-request-schema.hxx"

#include <drogon/orm/Field.h>

PortraitAccessRequestSchema::PortraitAccessRequestSchema(
    const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  portraitUserId = static_cast<int64_t>(row["portrait_user_id"].as<long long>());
  requesterUserId = static_cast<int64_t>(row["requester_user_id"].as<long long>());
  status = portraitAccessRequestStatusFromString(row["status"].as<std::string>());
  if (!row["resolved_by"].isNull())
    resolvedBy = static_cast<int64_t>(row["resolved_by"].as<long long>());
  if (!row["resolved_at"].isNull())
    resolvedAt = static_cast<int64_t>(row["resolved_at"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
}
