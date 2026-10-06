#pragma once

#include <cstdint>
#include <string>

namespace module_purge_query
{
inline constexpr const char* SELECT_OWNERS = "SELECT owner FROM module_purge WHERE module_id = ? ORDER BY owner";
inline constexpr const char* INSERT =
    "INSERT INTO module_purge (module_id, owner, purged_at) VALUES (?, ?, ?) "
    "ON CONFLICT (module_id, owner) DO NOTHING";
inline constexpr const char* DELETE_MODULE = "DELETE FROM module_purge WHERE module_id = ?";
}

struct ModulePurgeCreateInput
{
  std::string moduleId;
  std::string owner;
  std::int64_t at{0};
};
