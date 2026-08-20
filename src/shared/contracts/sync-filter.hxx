#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct SyncFilter
{
  std::optional<int64_t> startTime;
  std::optional<int64_t> startId;
  std::optional<int64_t> endTime;
};

namespace sync_query
{
struct SyncQueryParts
{
  std::string query;
  std::vector<std::string> args;
};

inline SyncQueryParts buildSyncQuery(const SyncFilter& filter,
                                     std::string_view queryBoth,
                                     std::string_view queryFrom,
                                     std::string_view queryAll,
                                     std::string_view queryAfterBoth = {},
                                     std::string_view queryAfterFrom = {})
{
  if (filter.startTime && filter.startId && !queryAfterBoth.empty()) {
    if (filter.endTime) {
      return {std::string(queryAfterBoth),
              {std::to_string(*filter.startTime),
               std::to_string(*filter.startTime),
               std::to_string(*filter.startId),
               std::to_string(*filter.endTime)}};
    }
    if (!queryAfterFrom.empty()) {
      return {std::string(queryAfterFrom),
              {std::to_string(*filter.startTime),
               std::to_string(*filter.startTime),
               std::to_string(*filter.startId)}};
    }
  }
  if (filter.startTime && filter.endTime) {
    return {std::string(queryBoth),
            {std::to_string(*filter.startTime),
             std::to_string(*filter.endTime)}};
  }
  if (filter.startTime) {
    return {std::string(queryFrom), {std::to_string(*filter.startTime)}};
  }
  return {std::string(queryAll), {}};
}
} // namespace sync_query
