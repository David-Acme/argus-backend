#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace action_command_query
{

inline constexpr std::string_view FIND_STATUS =
    "SELECT status, detail, response, fingerprint, updated_at, generation "
    "FROM action_command WHERE command_id = ?";

// Legacy fail-closed remap: a pre-fingerprint row that did not conclusively
// complete may have reached hardware, so it can never be re-armed.
inline constexpr std::string_view MIGRATE_LEGACY_STATUS =
    "UPDATE action_command SET status = CASE "
    "WHEN status IN ('claimed', 'executing', 'failed') THEN 'indeterminate' "
    "WHEN status = 'sent' THEN 'succeeded' ELSE status END "
    "WHERE status IN ('claimed', 'executing', 'failed', 'sent')";

inline constexpr std::string_view CLAIM =
    "INSERT OR IGNORE INTO action_command (command_id, kind, camera_id, "
    "status, detail, fingerprint, attempts, created_at, updated_at) VALUES "
    "(?, ?, ?, 'executing', '', ?, 1, ?, ?)";

// A failed execution is re-armed under a new generation; the returned
// generation fences the winner's settle.
inline constexpr std::string_view REARM_FAILED =
    "UPDATE action_command SET status = 'executing', attempts = attempts + 1, "
    "generation = generation + 1, updated_at = ? "
    "WHERE command_id = ? AND status = 'retryable_failed' AND generation = ? "
    "RETURNING generation";

// Only the claim that still owns the current generation may settle.
inline constexpr std::string_view SETTLE =
    "UPDATE action_command SET status = ?, detail = ?, response = ?, "
    "updated_at = ? WHERE command_id = ? AND status = 'executing' "
    "AND generation = ?";

// Boot reconciliation: a lost in-flight claim becomes indeterminate, never a
// silent second execution at runtime.
inline constexpr std::string_view RECONCILE_EXPIRED =
    "UPDATE action_command SET status = 'indeterminate', "
    "detail = 'crash_during_execution', updated_at = ? "
    "WHERE status = 'executing' AND updated_at <= ?";

inline constexpr std::string_view UPSERT_LEASE =
    "INSERT INTO siren_lease (camera_id, command_id, expires_at, created_at) "
    "VALUES (?, ?, ?, ?) ON CONFLICT(camera_id) DO UPDATE SET "
    "command_id = excluded.command_id, expires_at = excluded.expires_at";

inline constexpr std::string_view DELETE_LEASE =
    "DELETE FROM siren_lease WHERE camera_id = ?";

inline constexpr std::string_view EXPIRED_LEASES =
    "SELECT camera_id FROM siren_lease WHERE expires_at <= ?";

} // namespace action_command_query

struct ActionCommandClaimInput
{
  std::string commandId;
  std::string kind;
  int64_t cameraId{0};
  std::string fingerprint;
  int64_t at{0};
  int64_t leaseSeconds{60};
};

enum class ActionClaimKind : uint8_t
{
  New = 0,
  Retry,
  Completed,
  InFlight,
  Indeterminate,
  Conflict,
  Rejected
};

struct ActionClaim
{
  ActionClaimKind kind{ActionClaimKind::New};
  int64_t generation{0};
  std::string detail;
  std::string response;
};

struct ActionCommandRow
{
  bool found{false};
  std::string status;
  std::string detail;
  std::string response;
  int64_t generation{0};
};

struct ActionCommandResultInput
{
  std::string commandId;
  int64_t generation{0};
  std::string status;
  std::string detail;
  std::string response;
  int64_t at{0};
};

struct SirenLeaseInput
{
  int64_t cameraId{0};
  std::string commandId;
  int64_t expiresAt{0};
  int64_t at{0};
};
