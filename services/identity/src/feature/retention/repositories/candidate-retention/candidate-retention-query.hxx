#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>
#include <vector>

namespace candidate_retention_query
{

inline constexpr std::string_view RETIRE_STALE =
    "UPDATE person SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') WHERE id IN ("
    "SELECT id FROM person WHERE status = 'candidate' AND user_id IS NULL "
    "AND name = '' AND deleted_at IS NULL AND last_seen_at < ? "
    "ORDER BY last_seen_at ASC LIMIT ?) "
    "RETURNING id, deleted_at";

inline constexpr std::string_view EMBEDDING_IDS =
    "SELECT id FROM face_embedding "
    "WHERE person_id IN (SELECT value FROM json_each(?))";

inline constexpr std::string_view CROP_KEYS =
    "SELECT crop_key FROM face_embedding "
    "WHERE person_id IN (SELECT value FROM json_each(?)) AND crop_key != ''";

inline constexpr std::string_view DELETE_VISITS =
    "DELETE FROM person_visit "
    "WHERE person_id IN (SELECT value FROM json_each(?))";

inline constexpr std::string_view DELETE_CROP_CAPABILITIES =
    "DELETE FROM person_crop_capability "
    "WHERE person_id IN (SELECT value FROM json_each(?))";

inline constexpr std::string_view DELETE_EMBEDDINGS =
    "DELETE FROM face_embedding "
    "WHERE person_id IN (SELECT value FROM json_each(?))";

inline constexpr std::string_view DELETE_SNAPSHOTS =
    "DELETE FROM person_snapshot "
    "WHERE person_id IN (SELECT value FROM json_each(?))";

inline constexpr std::string_view DELETE_TAGS =
    "DELETE FROM person_tag "
    "WHERE person_id IN (SELECT value FROM json_each(?))";

}

struct CandidateRetireInput
{
  int64_t cutoff{0};
  int64_t limit{0};
  drogon::orm::DbClient* client{nullptr};
};

struct RetiredCandidate
{
  int64_t id{0};
  int64_t deletedAt{0};
};

struct RetiredBiometrics
{
  std::vector<int64_t> embeddingIds;
  std::vector<std::string> cropKeys;
};
