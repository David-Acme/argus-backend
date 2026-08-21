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
  /**
   * Set for the tables that belong to a user. Their queries close with the
   * ownership predicate, so the id is appended after the range arguments
   * `buildSyncQuery` produces (see `sync_query::withUser`).
   */
  std::optional<int64_t> userId;
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

/**
 * Appends the user argument once per `?` the ownership predicate spends. Kept
 * here so every user-scoped repository binds it the same way.
 */
inline SyncQueryParts withUser(SyncQueryParts parts,
                               const std::optional<int64_t>& userId,
                               int placeholders)
{
  // Fails closed: without a user the predicate binds 0, which no row can own,
  // so a caller that forgets the scope gets nothing rather than everything.
  const std::string value = std::to_string(userId.value_or(0));
  for (int i = 0; i < placeholders; ++i)
    parts.args.push_back(value);
  return parts;
}
} // namespace sync_query
