#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace agenda_announcement_query
{
inline constexpr std::string_view DUE_EVENTS =
    "SELECT e.id, e.owner_id, e.title, e.location, e.starts_at "
    "FROM calendar_event e WHERE e.deleted_at IS NULL AND e.is_all_day = 0 "
    "AND e.starts_at > ? AND e.starts_at <= ? AND NOT EXISTS ("
    "SELECT 1 FROM agenda_announcement a WHERE a.kind = 'event' "
    "AND a.ref_id = e.id AND a.occurrence_at = e.starts_at) "
    "ORDER BY e.starts_at ASC, e.id ASC LIMIT ?";

inline constexpr std::string_view SHARES_PREFIX =
    "SELECT calendar_event_id, user_id FROM calendar_event_share "
    "WHERE deleted_at IS NULL AND calendar_event_id IN (";

inline constexpr std::string_view DUE_REMINDERS =
    "SELECT r.id, r.target_user_id, r.title, r.description, r.scheduled_at "
    "FROM reminder r WHERE r.deleted_at IS NULL AND r.is_completed = 0 "
    "AND r.scheduled_at > ? AND r.scheduled_at <= ? AND NOT EXISTS ("
    "SELECT 1 FROM agenda_announcement a WHERE a.kind = 'reminder' "
    "AND a.ref_id = r.id AND a.occurrence_at = r.scheduled_at) "
    "ORDER BY r.scheduled_at ASC, r.id ASC LIMIT ?";

inline constexpr std::string_view RECORD =
    "INSERT OR IGNORE INTO agenda_announcement (kind, ref_id, occurrence_at, "
    "created_at) VALUES (?, ?, ?, ?)";

inline constexpr std::string_view PURGE =
    "DELETE FROM agenda_announcement WHERE occurrence_at < ?";
}

struct AgendaWindowInput
{
  int64_t after{0};
  int64_t until{0};
  int limit{100};
};

struct AgendaRecordInput
{
  std::string kind;
  int64_t refId{0};
  int64_t occurrenceAt{0};
  int64_t at{0};
};
