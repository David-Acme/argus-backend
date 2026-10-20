#pragma once

#include <array>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>
#include <vector>

namespace agenda_announcement_query
{
inline constexpr std::string_view DUE_EVENTS =
    "WITH leads(minutes) AS (VALUES (0), (5), (10), (15), (30), (60)) "
    "SELECT e.id, e.owner_id, e.title, e.location, e.starts_at, l.minutes "
    "FROM calendar_event e JOIN leads l "
    "ON e.starts_at - l.minutes * 60 <= ? "
    "WHERE e.deleted_at IS NULL AND e.is_all_day = 0 "
    "AND COALESCE(e.recurrence_rule, '') = '' "
    "AND e.starts_at > ? AND e.starts_at <= ? AND NOT EXISTS ("
    "SELECT 1 FROM agenda_notice a WHERE a.kind = 'event' "
    "AND a.ref_id = e.id AND a.occurrence_at = e.starts_at "
    "AND a.lead_minutes = l.minutes) "
    "ORDER BY e.starts_at ASC, e.id ASC, l.minutes DESC LIMIT ?";

inline constexpr std::string_view LIVE_SERIES =
    "SELECT id, owner_id, title, location, starts_at, recurrence_rule, "
    "recurrence_exceptions FROM calendar_event "
    "WHERE deleted_at IS NULL AND is_all_day = 0 "
    "AND COALESCE(recurrence_rule, '') <> '' AND starts_at <= ? "
    "ORDER BY id ASC";

inline constexpr std::string_view EVENT_NOTICES_PREFIX =
    "SELECT ref_id, occurrence_at, lead_minutes FROM agenda_notice "
    "WHERE kind = 'event' AND occurrence_at > ? AND occurrence_at <= ? "
    "AND ref_id IN (";

inline constexpr std::string_view SHARES_PREFIX =
    "SELECT calendar_event_id, user_id FROM calendar_event_share "
    "WHERE deleted_at IS NULL AND calendar_event_id IN (";

inline constexpr std::string_view DUE_REMINDERS =
    "SELECT r.id, r.target_user_id, r.title, r.description, r.scheduled_at "
    "FROM reminder r WHERE r.deleted_at IS NULL AND r.is_completed = 0 "
    "AND r.scheduled_at > ? AND r.scheduled_at <= ? AND NOT EXISTS ("
    "SELECT 1 FROM agenda_notice a WHERE a.kind = 'reminder' "
    "AND a.ref_id = r.id AND a.occurrence_at = r.scheduled_at) "
    "ORDER BY r.scheduled_at ASC, r.id ASC LIMIT ?";

inline constexpr std::string_view RECORD =
    "INSERT OR IGNORE INTO agenda_notice (kind, ref_id, occurrence_at, "
    "lead_minutes, created_at) VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view PURGE =
    "DELETE FROM agenda_notice WHERE occurrence_at < ?";
}

inline constexpr int64_t kMaxLeadS = 3600;
inline constexpr std::array<int, 6> kLeadMinutes{60, 30, 15, 10, 5, 0};

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
  int leadMinutes{0};
  int64_t at{0};
  drogon::orm::DbClient* client{nullptr};
};

struct AgendaNoticesInput
{
  std::vector<int64_t> eventIds;
  int64_t after{0};
  int64_t until{0};
};
