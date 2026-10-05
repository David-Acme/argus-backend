#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>
#include <vector>

namespace biometric_erase_query
{
inline constexpr std::string_view EMBEDDINGS =
    "SELECT id, crop_key FROM face_embedding "
    "WHERE person_id IN (SELECT id FROM person WHERE user_id = ?)";

inline constexpr std::string_view PORTRAIT_KEYS =
    "SELECT object_key FROM stored_file WHERE category = 'portrait' "
    "AND deleted_at IS NULL AND (created_by = ?1 OR id IN "
    "(SELECT file_id FROM user_portrait WHERE user_id = ?1))";

inline constexpr std::string_view RETIRE_PORTRAIT_FILES =
    "UPDATE stored_file SET deleted_at = strftime('%s', 'now') "
    "WHERE category = 'portrait' AND deleted_at IS NULL AND (created_by = ?1 OR "
    "id IN (SELECT file_id FROM user_portrait WHERE user_id = ?1))";

inline constexpr std::string_view DELETE_PORTRAIT_LINK =
    "DELETE FROM user_portrait WHERE user_id = ?";

inline constexpr std::string_view DELETE_PORTRAIT_CAPABILITIES =
    "DELETE FROM portrait_preview_capability WHERE portrait_user_id = ?";

inline constexpr std::string_view DELETE_CROP_CAPABILITIES =
    "DELETE FROM person_crop_capability "
    "WHERE person_id IN (SELECT id FROM person WHERE user_id = ?)";

inline constexpr std::string_view DELETE_SNAPSHOTS =
    "DELETE FROM person_snapshot "
    "WHERE person_id IN (SELECT id FROM person WHERE user_id = ?)";

inline constexpr std::string_view DELETE_EMBEDDINGS =
    "DELETE FROM face_embedding "
    "WHERE person_id IN (SELECT id FROM person WHERE user_id = ?)";

inline constexpr std::string_view DELETE_PRIVACY =
    "DELETE FROM user_privacy WHERE user_id = ?";
}

struct BiometricEraseInput
{
  int64_t userId{0};
  drogon::orm::DbClient* client{nullptr};
};

struct ErasedBiometrics
{
  std::vector<int64_t> embeddingIds;
  std::vector<std::string> objectKeys;
  std::size_t portraits{0};
  bool hadPrivacy{false};
};
