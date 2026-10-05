#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <identity/person-category.hxx>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sighting_query
{
inline constexpr std::string_view FIND_PERSONS_PREFIX =
    "SELECT id, user_id, name, category, status, visit_count, visitor_number "
    "FROM person WHERE deleted_at IS NULL AND id IN (";

inline constexpr std::string_view FIND_SAMPLES =
    "SELECT id, embedding, quality, crop_key FROM face_embedding "
    "WHERE person_id = ? AND model = ? ORDER BY id";

inline constexpr std::string_view NEXT_VISITOR_NUMBER =
    "UPDATE visitor_counter SET last_number = MAX(last_number, "
    "(SELECT COALESCE(MAX(visitor_number), 0) FROM person)) + 1 "
    "WHERE id = 1 RETURNING last_number";

inline constexpr std::string_view INSERT_VISITOR =
    "INSERT INTO person (user_id, name, alias, observation, status, category, "
    "visitor_number, visit_count, first_seen_at, last_seen_at) "
    "VALUES (NULL, '', '', '', 'candidate', '', ?, 0, ?, ?)";

inline constexpr std::string_view INSERT_SAMPLE =
    "INSERT INTO face_embedding (person_id, embedding, angle_label, quality, "
    "model, camera_id, crop_key) VALUES (?, ?, 'camera', ?, ?, ?, '')";

inline constexpr std::string_view DELETE_SAMPLE =
    "DELETE FROM face_embedding WHERE id = ?";

inline constexpr std::string_view SET_SAMPLE_CROP =
    "UPDATE face_embedding SET crop_key = ? WHERE id = ?";

inline constexpr std::string_view FIND_OPEN_VISIT =
    "SELECT id FROM person_visit WHERE person_id = ? AND camera_id = ? "
    "AND last_seen_at >= ? ORDER BY last_seen_at DESC LIMIT 1";

inline constexpr std::string_view EXTEND_VISIT =
    "UPDATE person_visit SET last_seen_at = MAX(last_seen_at, ?), "
    "sightings = sightings + 1 WHERE id = ?";

inline constexpr std::string_view INSERT_VISIT =
    "INSERT INTO person_visit (person_id, camera_id, started_at, last_seen_at) "
    "VALUES (?, ?, ?, ?)";

inline constexpr std::string_view COUNT_VISIT =
    "UPDATE person SET visit_count = visit_count + 1, "
    "last_seen_at = MAX(last_seen_at, ?), updated_at = strftime('%s', 'now') "
    "WHERE id = ?";

inline constexpr std::string_view TOUCH_PERSON =
    "UPDATE person SET last_seen_at = MAX(last_seen_at, ?) WHERE id = ?";
}

struct SightingPerson
{
  int64_t id{0};
  std::optional<int64_t> userId;
  std::string name;
  PersonCategory category{PersonCategory::None};
  bool known{false};
  int64_t visitCount{0};
  std::optional<int64_t> visitorNumber;
};

struct SightingSample
{
  int64_t id{0};
  std::vector<float> embedding;
  float quality{0.0F};
  std::string cropKey;
};

struct SightingSampleInsertInput
{
  int64_t personId{0};
  const std::vector<float>& embedding;
  float quality{0.0F};
  int64_t cameraId{0};
};

struct SightingVisitInput
{
  int64_t personId{0};
  int64_t cameraId{0};
  int64_t at{0};
  int64_t gapSeconds{0};
};
