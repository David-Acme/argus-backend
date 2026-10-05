#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <functional>
#include <string>

namespace outbox_query
{

inline constexpr const char* INSERT_EVENT =
    "INSERT OR IGNORE INTO change_outbox (event_id, subject, fingerprint, "
    "payload, status, attempts, created_at, sent_at) "
    "VALUES (?, ?, ?, ?, ?, 0, ?, 0)";

inline constexpr const char* FIND_FINGERPRINT =
    "SELECT fingerprint FROM change_outbox WHERE event_id = ?";

inline constexpr const char* PENDING_BATCH =
    "SELECT rowid AS row_id, COALESCE(event_id, '') AS event_id, subject, "
    "payload, attempts FROM change_outbox "
    "WHERE status = ? ORDER BY rowid ASC LIMIT ?";

inline constexpr const char* MARK_SENT =
    "UPDATE change_outbox SET status = ?, sent_at = ?, attempts = attempts + 1 "
    "WHERE rowid = ? AND status = ?";

inline constexpr const char* RECORD_ATTEMPT =
    "UPDATE change_outbox SET attempts = attempts + 1 "
    "WHERE rowid = ? AND status = ?";

inline constexpr const char* PURGE_SENT =
    "DELETE FROM change_outbox WHERE status = ? AND sent_at <= ?";

inline constexpr const char* COUNT_OUTBOX_TABLE =
    "SELECT COUNT(*) AS total FROM sqlite_master "
    "WHERE type = 'table' AND name = 'change_outbox'";

inline constexpr const char* COUNT_OUTBOX_COLUMN =
    "SELECT COUNT(*) AS total FROM pragma_table_info('change_outbox') "
    "WHERE name = ?";

inline constexpr const char* ADD_EVENT_ID_COLUMN =
    "ALTER TABLE change_outbox ADD COLUMN event_id TEXT NOT NULL DEFAULT ''";

inline constexpr const char* ADD_SUBJECT_COLUMN =
    "ALTER TABLE change_outbox ADD COLUMN subject TEXT NOT NULL DEFAULT ''";

}

namespace outbox
{

using ClientAccessor = std::function<drogon::orm::DbClientPtr()>;

enum class OutboxDisposition : uint8_t
{
  Enqueued = 0,
  Replay,
  Conflict,
};

struct OutboxInsert
{
  std::string eventId;
  std::string subject;
  std::string fingerprint;
  std::string payload;
  int64_t at{0};
  drogon::orm::DbClient* client{nullptr};
};

struct OutboxRow
{
  int64_t id{0};
  std::string eventId;
  std::string subject;
  std::string payload;
  int attempts{0};
};

}
