#pragma once

#include <sync/sync-filter.hxx>

#include <json/value.h>

#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tombstone_page
{
inline bool rereadsBoundary(const SyncFilter& filter)
{
  return filter.startTime.has_value() && filter.startId.has_value();
}

struct MergeInput
{
  std::vector<Json::Value> boundary;
  std::vector<Json::Value> page;
};

inline std::vector<Json::Value> merge(MergeInput input)
{
  std::vector<Json::Value> rows;
  rows.reserve(input.boundary.size() + input.page.size());
  std::unordered_set<int64_t> seen;
  for (auto* part : {&input.boundary, &input.page}) {
    for (auto& row : *part) {
      if (seen.insert(row["id"].asInt64()).second)
        rows.push_back(std::move(row));
    }
  }
  return rows;
}
}
