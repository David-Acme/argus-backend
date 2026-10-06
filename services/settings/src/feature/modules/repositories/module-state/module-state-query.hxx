#pragma once

#include <feature/modules/schemas/module-state.hxx>

#include <cstdint>
#include <optional>
#include <string>

namespace module_state_query
{
inline constexpr const char* SELECT_ALL =
    "SELECT module_id, lifecycle, data_purged_at, updated_at FROM module_state ORDER BY module_id";
inline constexpr const char* UPSERT =
    "INSERT INTO module_state (module_id, lifecycle, data_purged_at, created_at, updated_at) "
    "VALUES (?, ?, COALESCE(?, 0), ?, ?) "
    "ON CONFLICT (module_id) DO UPDATE SET lifecycle = excluded.lifecycle, updated_at = excluded.updated_at, "
    "data_purged_at = CASE WHEN ? IS NULL THEN module_state.data_purged_at ELSE excluded.data_purged_at END";
}

struct ModuleStateUpsertInput
{
  std::string moduleId;
  ModuleLifecycle lifecycle{ModuleLifecycle::NotInstalled};
  std::optional<std::int64_t> dataPurgedAt;
  std::int64_t at{0};
};
