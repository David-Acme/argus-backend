#pragma once

#include <cstdint>
#include <optional>
#include <shared/enums.hxx>
#include <string>

namespace job_query
{

constexpr const char* INSERT_JOB =
    "INSERT INTO job (queue, payload, state, priority, attempts, "
    "max_attempts, dedupe_key, last_error, next_run_at, created_at, "
    "updated_at) VALUES (?, ?, ?, ?, 0, ?, ?, NULL, ?, ?, ?)";

constexpr const char* FIND_ID_BY_DEDUPE =
    "SELECT id FROM job WHERE queue = ? AND dedupe_key = ? LIMIT 1";

constexpr const char* LOAD_DUE =
    "SELECT id, queue, payload, priority, attempts, max_attempts, "
    "created_at FROM job WHERE queue = ? AND state IN ('waiting','delayed') "
    "AND (next_run_at IS NULL OR next_run_at <= ?) "
    "ORDER BY priority DESC, id ASC";

constexpr const char* MARK_ACTIVE =
    "UPDATE job SET state = 'active', updated_at = ? WHERE id = ?";

constexpr const char* MARK_DELAYED =
    "UPDATE job SET state = 'delayed', attempts = ?, last_error = ?, "
    "next_run_at = ?, updated_at = ? WHERE id = ?";

constexpr const char* MARK_FAILED =
    "UPDATE job SET state = 'failed', attempts = ?, last_error = ?, "
    "updated_at = ? WHERE id = ?";

constexpr const char* DELETE_JOB = "DELETE FROM job WHERE id = ?";

constexpr const char* RESET_ACTIVE =
    "UPDATE job SET state = 'waiting', updated_at = ? WHERE state = 'active'";

constexpr const char* COUNT_BY_STATE =
    "SELECT COUNT(*) FROM job WHERE queue = ? AND state = ?";

struct JobInsertInput
{
  std::string queue;
  std::string payload;
  int priority = 0;
  int maxAttempts = 3;
  std::string dedupeKey;
  int64_t nextRunAt = 0;
  int64_t createdAt = 0;
};

struct JobRow
{
  int64_t id = 0;
  std::string queue;
  std::string payload;
  int priority = 0;
  int attempts = 0;
  int maxAttempts = 3;
  int64_t createdAt = 0;
};

} // namespace job_query
