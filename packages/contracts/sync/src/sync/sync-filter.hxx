#pragma once

#include <cstddef>
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
  std::optional<int64_t> userId;
  std::vector<int64_t> scopeIds;
};

namespace sync_query
{
struct SyncQueryParts
{
  std::string query;
  std::vector<std::string> args;
};

struct BuildSyncQueryInput
{
  const SyncFilter& filter;
  std::string_view queryBoth;
  std::string_view queryFrom;
  std::string_view queryAll;
  std::string_view queryAfterBoth = {};
  std::string_view queryAfterFrom = {};
};

inline SyncQueryParts buildSyncQuery(const BuildSyncQueryInput& input)
{
  const SyncFilter& filter = input.filter;
  const std::string_view queryBoth = input.queryBoth;
  const std::string_view queryFrom = input.queryFrom;
  const std::string_view queryAll = input.queryAll;
  const std::string_view queryAfterBoth = input.queryAfterBoth;
  const std::string_view queryAfterFrom = input.queryAfterFrom;

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

struct WithUserInput
{
  SyncQueryParts parts;
  const std::optional<int64_t>& userId;
  int placeholders;
};

inline SyncQueryParts withUser(const WithUserInput& input)
{
  SyncQueryParts parts = std::move(input.parts);
  const std::optional<int64_t>& userId = input.userId;
  const int placeholders = input.placeholders;

  const std::string value = std::to_string(userId.value_or(0));
  for (int i = 0; i < placeholders; ++i)
    parts.args.push_back(value);
  return parts;
}
struct BuildScopedQueryInput
{
  const SyncFilter& filter;
  std::string_view head;
  std::string_view after;
  std::string_view tail;
};

inline SyncQueryParts buildScopedQuery(const BuildScopedQueryInput& input)
{
  const SyncFilter& filter = input.filter;
  SyncQueryParts parts;
  parts.query.reserve(input.head.size() + input.after.size() +
                      input.tail.size() + (filter.scopeIds.size() * 2) + 1);
  parts.args.reserve(filter.scopeIds.size() + 3);
  parts.query.append(input.head);
  for (std::size_t i = 0; i < filter.scopeIds.size(); ++i) {
    parts.query.append(i == 0 ? "?" : ",?");
    parts.args.push_back(std::to_string(filter.scopeIds[i]));
  }
  parts.query.push_back(')');
  if (filter.startTime) {
    parts.query.append(input.after);
    parts.args.push_back(std::to_string(*filter.startTime));
    parts.args.push_back(std::to_string(*filter.startTime));
    parts.args.push_back(std::to_string(filter.startId.value_or(0)));
  }
  parts.query.append(input.tail);
  return parts;
}
}
