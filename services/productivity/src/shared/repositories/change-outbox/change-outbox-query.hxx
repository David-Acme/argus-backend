#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>

namespace change_outbox_query
{

inline constexpr const char* INSERT_EVENT =
    "INSERT OR IGNORE INTO change_outbox (event_id, fingerprint, payload, "
    "status, attempts, created_at, sent_at) VALUES (?, ?, ?, ?, 0, ?, 0)";

inline constexpr const char* FIND_FINGERPRINT =
    "SELECT fingerprint FROM change_outbox WHERE event_id = ?";

inline constexpr const char* PENDING_BATCH =
    "SELECT event_id, payload, attempts FROM change_outbox "
    "WHERE status = ? ORDER BY created_at ASC, rowid ASC LIMIT ?";

inline constexpr const char* MARK_SENT =
    "UPDATE change_outbox SET status = ?, sent_at = ?, attempts = attempts + 1 "
    "WHERE event_id = ? AND status = ?";

inline constexpr const char* RECORD_ATTEMPT =
    "UPDATE change_outbox SET attempts = attempts + 1 "
    "WHERE event_id = ? AND status = ?";

inline constexpr const char* PURGE_SENT =
    "DELETE FROM change_outbox WHERE status = ? AND sent_at <= ?";

}

enum class ChangeOutboxDisposition : uint8_t
{
  Enqueued = 0,
  Replay,
  Conflict,
};

struct ChangeOutboxEnqueueInput
{
  std::string eventId;
  std::string fingerprint{};
  std::string payload;
  int64_t at{0};
  drogon::orm::DbClient* client{nullptr};
};

struct ChangeOutboxRow
{
  std::string eventId;
  std::string payload;
  int attempts{0};
};
