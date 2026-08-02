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
  std::optional<int64_t> endTime;
};

namespace sync_query
{
struct SyncQueryParts
{
  std::string query;
  std::vector<std::string> args;
};

// Selecciona la variante de query (rango completo / desde / todo) y arma los
// argumentos de forma sincrónica. No es una coroutine: se ejecuta antes de
// cualquier `co_await`, así que no hay referencias colgadas en la suspensión.
inline SyncQueryParts buildSyncQuery(const SyncFilter& filter,
                                     std::string_view queryBoth,
                                     std::string_view queryFrom,
                                     std::string_view queryAll)
{
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
