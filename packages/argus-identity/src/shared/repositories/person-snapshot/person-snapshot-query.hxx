#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace person_snapshot_query
{
inline constexpr std::string_view UPSERT =
    "INSERT INTO person_snapshot (person_id, image, created_at) "
    "VALUES (?, ?, ?) ON CONFLICT(person_id) DO UPDATE SET "
    "image = excluded.image, created_at = excluded.created_at";

inline constexpr std::string_view FIND_BY_PERSON =
    "SELECT image FROM person_snapshot WHERE person_id = ?";
} // namespace person_snapshot_query

struct PersonSnapshotStoreInput
{
  int64_t personId{0};
  std::string image;
};
