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
} // namespace face_embedding_query

struct FaceEmbeddingCreateInput
{
  int64_t personId{0};
  std::string embedding;
  std::string angleLabel{"frontal"};
  double quality{1.0};
};
