#pragma once

#include <cstdint>
#include <string>

namespace change_outbox_query
{

// The transition-addressed write: the four legs that name an event by its own
// payload. `INSERT OR IGNORE` plus the UNIQUE index is the dedup.
inline constexpr const char* INSERT_EVENT =
    "INSERT OR IGNORE INTO change_outbox (event_id, subject, fingerprint, "
    "payload, status, attempts, created_at, sent_at) VALUES (?, ?, ?, ?, ?, 0, "
    "?, 0)";

// The journal write: it names the row's own position instead, so it leaves
// `event_id` NULL by omitting the column rather than binding a NULL, and it
// never collides — SQLite treats NULLs as distinct under a UNIQUE index.
inline constexpr const char* INSERT_ACTION =
    "INSERT INTO change_outbox (subject, fingerprint, payload, status, "
    "attempts, created_at, sent_at) VALUES (?, ?, ?, ?, 0, ?, 0)";

inline constexpr const char* FIND_FINGERPRINT =
    "SELECT fingerprint FROM change_outbox WHERE event_id = ?";

// Ordered by the row's own id, not the copies' `created_at ASC, rowid ASC`:
// with an AUTOINCREMENT id the two orders are the same one and this one has no
// one-second tie to break.
inline constexpr const char* PENDING_BATCH =
    "SELECT id, event_id, subject, payload, attempts FROM change_outbox "
    "WHERE status = ? ORDER BY id ASC LIMIT ?";

inline constexpr const char* MARK_SENT =
    "UPDATE change_outbox SET status = ?, sent_at = ?, attempts = attempts + 1 "
    "WHERE id = ? AND status = ?";

inline constexpr const char* RECORD_ATTEMPT =
    "UPDATE change_outbox SET attempts = attempts + 1 "
    "WHERE id = ? AND status = ?";

} // namespace change_outbox_query

enum class ChangeOutboxDisposition : uint8_t
{
  Enqueued = 0,
  Replay,
  Conflict,
  Failed,
};

struct ChangeOutboxEnqueueInput
{
  std::string eventId;
  std::string subject;
  std::string fingerprint;
  std::string payload;
  int64_t at{0};
};

struct ChangeOutboxActionInput
{
  std::string subject;
  std::string fingerprint;
  std::string payload;
  int64_t at{0};
};

struct ChangeOutboxRow
{
  int64_t id{0};
  std::string eventId;
  std::string subject;
  std::string payload;
  int attempts{0};
};
