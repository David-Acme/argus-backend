#pragma once

#include "sync-filter.hxx"

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <vector>

class Syncable
{
public:
  virtual ~Syncable() = default;

  [[nodiscard]] virtual drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const = 0;

  [[nodiscard]] virtual drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const = 0;

  [[nodiscard]] virtual drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const = 0;

  [[nodiscard]] virtual drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const = 0;
};
