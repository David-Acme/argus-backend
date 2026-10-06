#pragma once

#include <feature/modules/schemas/module-job.hxx>

#include <cstdint>
#include <optional>
#include <string>

namespace module_job_query
{
inline constexpr const char* COLUMNS =
    "SELECT id, module_id, kind, owner, state, reason, bytes_done, bytes_total, requested_by, created_at, updated_at, state_since "
    "FROM module_job ";
inline constexpr const char* WHERE_ID = "WHERE id = ?";
inline constexpr const char* WHERE_OPEN = "WHERE state NOT IN ('done', 'failed', 'cancelled') ORDER BY id";
inline constexpr const char* WHERE_LATEST =
    "WHERE id IN (SELECT MAX(id) FROM module_job GROUP BY module_id) ORDER BY id";
inline constexpr const char* INSERT =
    "INSERT INTO module_job (module_id, kind, owner, state, reason, bytes_done, bytes_total, "
    "requested_by, created_at, updated_at, state_since) VALUES (?, ?, '', 'queued', '', 0, ?, ?, ?, ?, ?)";
inline constexpr const char* UPDATE_PREFIX = "UPDATE module_job SET updated_at = ?";
inline constexpr const char* UPDATE_COL_STATE = ", state = ?, state_since = ?";
inline constexpr const char* UPDATE_COL_REASON = ", reason = ?";
inline constexpr const char* UPDATE_COL_OWNER = ", owner = ?";
inline constexpr const char* UPDATE_COL_BYTES_DONE = ", bytes_done = ?";
inline constexpr const char* UPDATE_COL_BYTES_TOTAL = ", bytes_total = ?";
inline constexpr const char* UPDATE_SUFFIX = " WHERE id = ?";
}

struct ModuleJobCreateInput
{
  std::string moduleId;
  JobKind kind{JobKind::Install};
  std::int64_t bytesTotal{0};
  std::int64_t requestedBy{0};
  std::int64_t at{0};
};

struct ModuleJobUpdateInput
{
  std::int64_t at{0};
  std::optional<JobState> state;
  std::optional<std::string> reason;
  std::optional<std::string> owner;
  std::optional<std::int64_t> bytesDone;
  std::optional<std::int64_t> bytesTotal;
};
