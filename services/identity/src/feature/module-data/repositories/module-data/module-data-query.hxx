#pragma once

#include <array>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>
#include <vector>

namespace module_data_query
{

inline constexpr std::string_view SUMMARY =
    "SELECT "
    "(SELECT COUNT(*) FROM person WHERE user_id IS NULL AND deleted_at IS NULL) AS visitors, "
    "(SELECT COUNT(*) FROM face_embedding WHERE person_id IN (SELECT id FROM person WHERE user_id IS NULL)) "
    "AS samples, "
    "(SELECT COUNT(*) FROM person_visit WHERE person_id IN (SELECT id FROM person WHERE user_id IS NULL)) "
    "AS visits, "
    "(SELECT COALESCE(SUM(?1 + length(name) + length(note) + length(alias) + length(observation)), 0) "
    "FROM person WHERE user_id IS NULL) + "
    "(SELECT COALESCE(SUM(?1 + length(embedding) + length(crop_key)), 0) FROM face_embedding "
    "WHERE person_id IN (SELECT id FROM person WHERE user_id IS NULL)) + "
    "(SELECT COUNT(*) * ?1 FROM person_visit WHERE person_id IN (SELECT id FROM person WHERE user_id IS NULL)) + "
    "(SELECT COALESCE(SUM(?1 + length(image)), 0) FROM person_snapshot "
    "WHERE person_id IN (SELECT id FROM person WHERE user_id IS NULL)) AS bytes";

inline constexpr std::string_view VISITOR_IDS = "SELECT id FROM person WHERE user_id IS NULL";

inline constexpr std::string_view EMBEDDING_IDS =
    "SELECT id FROM face_embedding WHERE person_id IN (SELECT value FROM json_each(?))";

inline constexpr std::string_view CROP_KEYS =
    "SELECT crop_key FROM face_embedding "
    "WHERE person_id IN (SELECT value FROM json_each(?)) AND crop_key != ''";

inline constexpr std::array<std::string_view, 6> DELETE_VISITORS{
    "DELETE FROM person_crop_capability WHERE person_id IN (SELECT value FROM json_each(?))",
    "DELETE FROM person_visit WHERE person_id IN (SELECT value FROM json_each(?))",
    "DELETE FROM face_embedding WHERE person_id IN (SELECT value FROM json_each(?))",
    "DELETE FROM person_snapshot WHERE person_id IN (SELECT value FROM json_each(?))",
    "DELETE FROM person_tag WHERE person_id IN (SELECT value FROM json_each(?))",
    "DELETE FROM person WHERE user_id IS NULL AND id IN (SELECT value FROM json_each(?))"};

inline constexpr std::string_view RESET_COUNTER = "UPDATE visitor_counter SET last_number = 0 WHERE id = 1";

inline constexpr std::int64_t kRowOverheadBytes = 64;

}

struct PurgedVisitors
{
  std::int64_t visitors{0};
  std::vector<std::int64_t> embeddingIds;
  std::vector<std::string> cropKeys;
};
