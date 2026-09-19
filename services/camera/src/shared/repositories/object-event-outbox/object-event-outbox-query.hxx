#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace object_event_outbox_query
{

inline constexpr std::string_view SELECT_COOLDOWN =
    "SELECT last_emit_ms FROM camera_event_cooldown "
    "WHERE camera_id = ? AND class = ?";

inline constexpr std::string_view UPSERT_COOLDOWN =
    "INSERT INTO camera_event_cooldown (camera_id, class, last_emit_ms) "
    "VALUES (?, ?, ?) ON CONFLICT(camera_id, class) DO UPDATE SET "
    "last_emit_ms = excluded.last_emit_ms";

inline constexpr std::string_view INSERT_EVENT =
    "INSERT OR IGNORE INTO object_event_outbox (event_id, payload, status, "
    "attempts, created_at, sent_at) VALUES (?, ?, ?, 0, ?, 0)";

inline constexpr std::string_view COUNT_PENDING =
    "SELECT COUNT(*) AS total FROM object_event_outbox WHERE status = ?";

inline constexpr std::string_view OLDEST_PENDING =
    "SELECT event_id FROM object_event_outbox WHERE status = ? "
    "ORDER BY created_at ASC LIMIT 1";

inline constexpr std::string_view MARK_OVERFLOW =
    "UPDATE object_event_outbox SET status = ?, sent_at = ? "
    "WHERE event_id = ?";

inline constexpr std::string_view NEXT_PENDING =
    "SELECT event_id, payload, attempts FROM object_event_outbox "
    "WHERE status = ? ORDER BY created_at ASC LIMIT 1";

inline constexpr std::string_view MARK_SENT =
    "UPDATE object_event_outbox SET status = ?, sent_at = ?, "
    "attempts = attempts + 1 WHERE event_id = ? AND status = ?";

inline constexpr std::string_view RECORD_ATTEMPT =
    "UPDATE object_event_outbox SET attempts = attempts + 1 "
    "WHERE event_id = ? AND status = ?";

inline constexpr std::string_view PURGE_COOLDOWNS =
    "DELETE FROM camera_event_cooldown WHERE last_emit_ms < ?";

inline constexpr std::string_view OUTBOX_STATS =
    "SELECT SUM(CASE WHEN status = ? THEN 1 ELSE 0 END) AS pending, "
    "SUM(CASE WHEN status = ? THEN 1 ELSE 0 END) AS sent, "
    "SUM(CASE WHEN status = ? THEN 1 ELSE 0 END) AS dropped, "
    "MIN(CASE WHEN status = ? THEN created_at END) AS oldest "
    "FROM object_event_outbox";

} // namespace object_event_outbox_query

enum class ObjectEventEnqueueResult : uint8_t
{
  Recorded = 0,
  Suppressed,
  Failed,
};

struct ObjectEventEnqueueOutcome
{
  ObjectEventEnqueueResult result{ObjectEventEnqueueResult::Failed};
  bool inserted{false};
  int64_t overflowDropped{0};
  int64_t netPendingDelta{0};
};

struct ObjectEventEnqueueInput
{
  std::string eventId;
  std::string payload;
  int64_t cameraId{0};
  std::vector<std::string> cooldownClasses;
  int64_t nowMs{0};
  int64_t cooldownMs{0};
  int64_t maxPending{5000};
};

struct ObjectEventRow
{
  std::string eventId;
  std::string payload;
  int attempts{0};
};

struct ObjectEventOutboxStats
{
  int64_t pending{0};
  int64_t sent{0};
  int64_t overflowDropped{0};
  int64_t oldestPendingAtMs{0};
};
