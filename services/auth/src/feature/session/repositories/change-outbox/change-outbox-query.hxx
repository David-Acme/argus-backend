#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>

namespace change_outbox_query
{

inline constexpr const char* INSERT_ACTION =
    "INSERT INTO change_outbox (event_id, subject, fingerprint, payload, "
    "status, attempts, created_at, sent_at) VALUES (?, ?, ?, ?, ?, 0, ?, 0)";

inline constexpr const char* PENDING_BATCH =
    "SELECT id, event_id, subject, payload, attempts FROM change_outbox "
    "WHERE status = ? ORDER BY id ASC LIMIT ?";

inline constexpr const char* MARK_SENT =
    "UPDATE change_outbox SET status = ?, sent_at = ?, attempts = attempts + 1 "
    "WHERE id = ? AND status = ?";

inline constexpr const char* RECORD_ATTEMPT =
    "UPDATE change_outbox SET attempts = attempts + 1 "
    "WHERE id = ? AND status = ?";

inline constexpr const char* PURGE_SENT =
    "DELETE FROM change_outbox WHERE status = ? AND sent_at <= ?";

inline constexpr const char* COUNT_OUTBOX_TABLE =
    "SELECT COUNT(*) AS total FROM sqlite_master "
    "WHERE type = 'table' AND name = 'change_outbox'";

inline constexpr const char* COUNT_EVENT_ID_COLUMN =
    "SELECT COUNT(*) AS total FROM pragma_table_info('change_outbox') "
    "WHERE name = 'event_id'";

inline constexpr const char* ADD_EVENT_ID_COLUMN =
    "ALTER TABLE change_outbox ADD COLUMN event_id TEXT NOT NULL DEFAULT ''";

}

struct ChangeOutboxActionInput
{
  std::string eventId;
  std::string subject;
  std::string fingerprint;
  std::string payload;
  int64_t at{0};
  drogon::orm::DbClient* client{nullptr};
};

struct ChangeOutboxRow
{
  int64_t id{0};
  std::string eventId;
  std::string subject;
  std::string payload;
  int attempts{0};
};
