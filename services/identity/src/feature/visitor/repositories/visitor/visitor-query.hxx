#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <identity/person-category.hxx>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace visitor_query
{
inline constexpr std::string_view LIST =
    "SELECT p.id, p.name, p.category, p.note, p.status, p.visitor_number, "
    "p.visit_count, p.first_seen_at, p.last_seen_at, "
    "(SELECT COUNT(*) FROM face_embedding f WHERE f.person_id = p.id "
    "AND f.model = ?1) AS sample_count, "
    "(SELECT f.id FROM face_embedding f WHERE f.person_id = p.id "
    "AND f.model = ?1 AND f.crop_key != '' ORDER BY f.quality DESC, f.id DESC "
    "LIMIT 1) AS cover_sample_id, "
    "(SELECT group_concat(camera_id) FROM (SELECT DISTINCT v.camera_id "
    "FROM person_visit v WHERE v.person_id = p.id)) AS camera_ids "
    "FROM person p WHERE p.deleted_at IS NULL AND p.user_id IS NULL "
    "AND (?2 = 0 OR p.name != '') "
    "AND (?4 = 'all' OR (?4 = 'named' AND p.name != '') "
    "OR (?4 = 'unnamed' AND p.name = '') "
    "OR (?4 = 'watchlist' AND p.category = 'watchlist')) "
    "AND (?5 = '' OR instr(lower(p.name), ?5) > 0 "
    "OR instr(lower(p.note), ?5) > 0 "
    "OR instr(CAST(p.visitor_number AS TEXT), ?5) > 0) "
    "AND (p.last_seen_at, p.id) < (?6, ?7) "
    "ORDER BY p.last_seen_at DESC, p.id DESC LIMIT ?3";

inline constexpr std::string_view FIND =
    "SELECT p.id, p.name, p.category, p.note, p.status, p.visitor_number, "
    "p.visit_count, p.first_seen_at, p.last_seen_at, "
    "(SELECT COUNT(*) FROM face_embedding f WHERE f.person_id = p.id "
    "AND f.model = ?2) AS sample_count, "
    "(SELECT f.id FROM face_embedding f WHERE f.person_id = p.id "
    "AND f.model = ?2 AND f.crop_key != '' ORDER BY f.quality DESC, f.id DESC "
    "LIMIT 1) AS cover_sample_id, "
    "(SELECT group_concat(camera_id) FROM (SELECT DISTINCT v.camera_id "
    "FROM person_visit v WHERE v.person_id = p.id)) AS camera_ids "
    "FROM person p WHERE p.id = ?1 AND p.deleted_at IS NULL "
    "AND p.user_id IS NULL";

inline constexpr std::string_view SAMPLES =
    "SELECT id, quality, camera_id, crop_key != '' AS has_crop, created_at, "
    "embedding FROM face_embedding WHERE person_id = ? AND model = ? "
    "ORDER BY quality DESC, id DESC";

inline constexpr std::string_view VISITS =
    "SELECT id, camera_id, started_at, last_seen_at, sightings FROM person_visit "
    "WHERE person_id = ? ORDER BY started_at DESC LIMIT ?";

inline constexpr std::string_view VISIT_TIMES =
    "SELECT started_at FROM person_visit WHERE person_id = ? "
    "ORDER BY started_at DESC LIMIT 60";

inline constexpr std::string_view UPDATE_PREFIX = "UPDATE person SET ";
inline constexpr std::string_view UPDATE_COL_NAME = "name = ?";
inline constexpr std::string_view UPDATE_COL_STATUS = "status = ?";
inline constexpr std::string_view UPDATE_COL_CATEGORY = "category = ?";
inline constexpr std::string_view UPDATE_COL_NOTE = "note = ?";
inline constexpr std::string_view UPDATE_SUFFIX =
    ", updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL "
    "AND user_id IS NULL";

inline constexpr std::string_view SAMPLE_OWNERS_PREFIX =
    "SELECT id, person_id, crop_key, embedding FROM face_embedding WHERE model = ? "
    "AND id IN (";

inline constexpr std::string_view MOVE_SAMPLES_PREFIX =
    "UPDATE face_embedding SET person_id = ? WHERE id IN (";

inline constexpr std::string_view MOVE_ALL_SAMPLES_PREFIX =
    "UPDATE face_embedding SET person_id = ? WHERE person_id IN (";

inline constexpr std::string_view MOVE_VISITS_PREFIX =
    "UPDATE person_visit SET person_id = ? WHERE person_id IN (";

inline constexpr std::string_view SAMPLE_IDS_OF_PREFIX =
    "SELECT id, crop_key FROM face_embedding WHERE person_id IN (";

inline constexpr std::string_view DELETE_PERSONS_PREFIX =
    "DELETE FROM person WHERE user_id IS NULL AND id IN (";

inline constexpr std::string_view REFRESH_COUNTS =
    "UPDATE person SET "
    "visit_count = (SELECT COUNT(*) FROM person_visit WHERE person_id = ?1), "
    "first_seen_at = COALESCE((SELECT MIN(started_at) FROM person_visit "
    "WHERE person_id = ?1), first_seen_at), "
    "last_seen_at = COALESCE((SELECT MAX(last_seen_at) FROM person_visit "
    "WHERE person_id = ?1), last_seen_at), "
    "updated_at = strftime('%s', 'now') WHERE id = ?1";

inline constexpr std::string_view VISITS_FROM_SAMPLES_PREFIX =
    "INSERT INTO person_visit (person_id, camera_id, started_at, last_seen_at) "
    "SELECT ?, COALESCE(camera_id, 0), created_at, created_at FROM face_embedding "
    "WHERE id IN (";

inline constexpr std::string_view INSERT_VISITOR =
    "INSERT INTO person (user_id, name, alias, observation, status, category, "
    "visitor_number, visit_count, first_seen_at, last_seen_at) "
    "VALUES (NULL, '', '', '', 'candidate', '', "
    "(SELECT COALESCE(MAX(visitor_number), 0) + 1 FROM person), 0, "
    "strftime('%s', 'now'), strftime('%s', 'now'))";

inline constexpr std::string_view DELETE_SAMPLES_PREFIX =
    "DELETE FROM face_embedding WHERE id IN (";

inline constexpr std::string_view SET_SAMPLE_CROP =
    "UPDATE face_embedding SET crop_key = ? WHERE id = ?";

inline constexpr std::string_view SAMPLE_CROP =
    "SELECT crop_key FROM face_embedding WHERE id = ? AND person_id = ?";
}

struct VisitorRow
{
  int64_t id{0};
  std::string name;
  PersonCategory category{PersonCategory::None};
  std::string note;
  bool known{false};
  std::optional<int64_t> visitorNumber;
  int64_t visitCount{0};
  int64_t firstSeenAt{0};
  int64_t lastSeenAt{0};
  int64_t sampleCount{0};
  std::optional<int64_t> coverSampleId;
  std::vector<int64_t> cameraIds;
};

struct VisitorSampleRow
{
  int64_t id{0};
  double quality{0.0};
  std::optional<int64_t> cameraId;
  bool hasCrop{false};
  int64_t createdAt{0};
  std::vector<float> embedding;
};

struct VisitorVisitRow
{
  int64_t id{0};
  int64_t cameraId{0};
  int64_t startedAt{0};
  int64_t lastSeenAt{0};
  int64_t sightings{0};
};

enum class VisitorListFilter : std::uint8_t
{
  All,
  Named,
  Unnamed,
  Watchlist
};

[[nodiscard]] constexpr std::string_view visitorListFilterToString(VisitorListFilter filter)
{
  switch (filter)
  {
  case VisitorListFilter::Named:
    return "named";
  case VisitorListFilter::Unnamed:
    return "unnamed";
  case VisitorListFilter::Watchlist:
    return "watchlist";
  case VisitorListFilter::All:
    break;
  }
  return "all";
}

[[nodiscard]] constexpr std::optional<VisitorListFilter>
visitorListFilterFromString(std::string_view value)
{
  for (const auto filter : {VisitorListFilter::All, VisitorListFilter::Named,
                            VisitorListFilter::Unnamed, VisitorListFilter::Watchlist})
    if (visitorListFilterToString(filter) == value)
      return filter;
  return std::nullopt;
}

struct VisitorCursor
{
  int64_t lastSeenAt{0};
  int64_t id{0};
};

struct VisitorPageQuery
{
  VisitorListFilter filter{VisitorListFilter::All};
  std::string search;
  VisitorCursor after;
  int64_t limit{500};
};

struct VisitorListInput
{
  bool namedOnly{false};
  VisitorPageQuery page;
};

struct VisitorUpdateInput
{
  std::optional<std::string> name;
  std::optional<PersonCategory> category;
  std::optional<std::string> note;
};

struct VisitorSampleOwner
{
  int64_t id{0};
  int64_t personId{0};
  std::string cropKey;
  std::vector<float> embedding;
};

struct VisitorMergeInput
{
  int64_t targetId{0};
  std::vector<int64_t> sourceIds;
  drogon::orm::DbClient* client{nullptr};
};

struct VisitorSplitInput
{
  int64_t newPersonId{0};
  std::vector<int64_t> sampleIds;
  drogon::orm::DbClient* client{nullptr};
};

struct VisitorRemoved
{
  std::vector<int64_t> sampleIds;
  std::vector<std::string> cropKeys;
};

struct VisitorRemoveInput
{
  std::vector<int64_t> personIds;
  drogon::orm::DbClient* client{nullptr};
};

struct VisitorSampleCropInput
{
  int64_t sampleId{0};
  std::string cropKey;
};
