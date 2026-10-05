#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>

namespace crop_capability_query
{
inline constexpr std::string_view INSERT =
    "INSERT INTO person_crop_capability (token_hash, person_id, "
    "face_embedding_id, requester_user_id, expires_at) VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view CONSUME =
    "UPDATE person_crop_capability SET consumed_at = ?1 "
    "WHERE token_hash = ?2 AND requester_user_id = ?3 AND consumed_at IS NULL "
    "AND expires_at > ?1 RETURNING person_id, face_embedding_id";

inline constexpr std::string_view PURGE_EXPIRED =
    "DELETE FROM person_crop_capability WHERE expires_at < ?";
}

struct CropCapabilityCreateInput
{
  std::string tokenHash;
  int64_t personId{0};
  int64_t sampleId{0};
  int64_t requesterUserId{0};
  int64_t expiresAt{0};
};

struct CropCapabilityConsumeInput
{
  std::string tokenHash;
  int64_t requesterUserId{0};
  int64_t now{0};
};

struct ConsumedCropCapability
{
  int64_t personId{0};
  int64_t sampleId{0};
};
