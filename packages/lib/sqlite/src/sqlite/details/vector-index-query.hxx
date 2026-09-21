#pragma once

#include <string>

namespace vector_index_query
{

inline std::string memoryVecDdl(int dims)
{
  return "CREATE VIRTUAL TABLE IF NOT EXISTS memory_vec USING vec0("
         "embedding float[" +
         std::to_string(dims) +
         "] distance_metric=cosine, "
         "partition TEXT PARTITION KEY, memory_id INTEGER, view INTEGER);";
}

inline constexpr const char* FACE_VEC_DDL =
    "CREATE VIRTUAL TABLE IF NOT EXISTS face_vec USING vec0("
    "embedding float[128] distance_metric=cosine, "
    "person_id INTEGER, face_embedding_id INTEGER)";

inline constexpr const char* DROP_MEMORY_VEC =
    "DROP TABLE IF EXISTS memory_vec";

inline constexpr const char* DROP_FACE_VEC = "DROP TABLE IF EXISTS face_vec";

inline constexpr const char* TABLE_SQL =
    "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = ?";

} // namespace vector_index_query
