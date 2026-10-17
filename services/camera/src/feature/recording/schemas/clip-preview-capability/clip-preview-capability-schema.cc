#include "clip-preview-capability-schema.hxx"

ClipPreviewCapabilitySchema::ClipPreviewCapabilitySchema(
    const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<int64_t>());
  tokenHash = row["token_hash"].as<std::string>();
  clipId = static_cast<int64_t>(row["clip_id"].as<int64_t>());
  requesterUserId = static_cast<int64_t>(row["requester_user_id"].as<int64_t>());
  expiresAt = static_cast<int64_t>(row["expires_at"].as<int64_t>());
  if (!row["consumed_at"].isNull())
    consumedAt = static_cast<int64_t>(row["consumed_at"].as<int64_t>());
  createdAt = static_cast<int64_t>(row["created_at"].as<int64_t>());
}
