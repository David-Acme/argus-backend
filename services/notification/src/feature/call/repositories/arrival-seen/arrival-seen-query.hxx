#pragma once

#include <cstdint>
#include <string_view>

namespace arrival_seen_query
{
inline constexpr std::string_view FIND =
    "SELECT last_seen FROM call_arrival_seen WHERE person_id = ?";

inline constexpr std::string_view UPSERT =
    "INSERT INTO call_arrival_seen (person_id, last_seen) VALUES (?, ?) "
    "ON CONFLICT(person_id) DO UPDATE SET last_seen = excluded.last_seen "
    "WHERE excluded.last_seen > call_arrival_seen.last_seen";
}

struct ArrivalSeenInput
{
  int64_t personId{0};
  int64_t seenAt{0};
};
