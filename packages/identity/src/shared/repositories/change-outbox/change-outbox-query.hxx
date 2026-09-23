#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>

namespace change_outbox_query
{

inline constexpr const char* INSERT_EVENT =
    "INSERT OR IGNORE INTO change_outbox (event_id, subject, fingerprint, "
    "payload, status, attempts, created_at, sent_at) VALUES (?, ?, ?, ?, ?, 0, "
    "?, 0)";

inline constexpr const char* INSERT_ACTION =
    "INSERT INTO change_outbox (subject, fingerprint, payload, status, "
    "attempts, created_at, sent_at) VALUES (?, ?, ?, ?, 0, ?, 0)";

inline constexpr const char* FIND_FINGERPRINT =
    "SELECT fingerprint FROM change_outbox WHERE event_id = ?";

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
  std::string subject;
  std::string fingerprint{};
  std::string payload;
  int64_t at{0};
  drogon::orm::DbClient* client{nullptr};
};

struct ChangeOutboxActionInput
{
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
