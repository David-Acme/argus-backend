#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace face_embedding_query
{
inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM face_embedding WHERE id = ?";

inline constexpr std::string_view FIND_BY_PERSON =
    "SELECT * FROM face_embedding WHERE person_id = ?";

inline constexpr std::string_view INSERT =
    "INSERT INTO face_embedding (person_id, embedding, angle_label, quality) "
    "VALUES (?, ?, ?, ?)";

inline constexpr std::string_view DELETE_BY_PERSON =
    "DELETE FROM face_embedding WHERE person_id = ?";

inline constexpr std::string_view FIND_ALL = "SELECT * FROM face_embedding";

inline constexpr std::string_view FIND_IDS_BY_PERSON =
    "SELECT id FROM face_embedding WHERE person_id = ?";

inline constexpr std::string_view VEC_INSERT =
    "INSERT INTO face_vec (rowid, embedding, person_id, face_embedding_id) "
    "VALUES (?, ?, ?, ?)";

inline constexpr std::string_view VEC_SEARCH =
    "SELECT person_id, distance FROM face_vec "
    "WHERE embedding MATCH ? ORDER BY distance LIMIT ?";

inline constexpr std::string_view VEC_DELETE =
    "DELETE FROM face_vec WHERE rowid = ?";

inline constexpr std::string_view VEC_COUNT = "SELECT COUNT(*) FROM face_vec";
} // namespace face_embedding_query

struct FaceEmbeddingCreateInput
{
  int64_t personId{0};
  std::string embedding;
  std::string angleLabel{"frontal"};
  double quality{1.0};
};

struct FaceVecInsertInput
{
  const float* embedding;
  int dims;
  int64_t personId;
  int64_t faceEmbeddingId;
};

struct FaceVecSearchInput
{
  const float* query;
  int dims;
  int topK;
};

struct FaceVecHit
{
  int64_t personId;
  float distance;
};
