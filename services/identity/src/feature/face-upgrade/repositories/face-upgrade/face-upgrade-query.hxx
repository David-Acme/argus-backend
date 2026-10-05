#pragma once

#include <cstdint>
#include <string_view>

namespace face_upgrade_query
{
inline constexpr std::string_view FIND_STALE_USER_PERSONS =
    "SELECT p.id AS person_id, p.user_id AS user_id FROM person p "
    "WHERE p.user_id IS NOT NULL AND p.deleted_at IS NULL "
    "AND EXISTS (SELECT 1 FROM face_embedding f "
    "WHERE f.person_id = p.id AND f.model != ?) "
    "AND NOT EXISTS (SELECT 1 FROM face_embedding f "
    "WHERE f.person_id = p.id AND f.model = ?) "
    "ORDER BY p.id";
}

struct FaceUpgradeCandidate
{
  int64_t personId{0};
  int64_t userId{0};
};
