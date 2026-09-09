#pragma once
#include <cstdint>
#include <shared/enums.hxx>
#include <string>
#include <string_view>

namespace event_query
{
inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM event WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND_RECENT =
    "SELECT * FROM event WHERE deleted_at IS NULL "
    "ORDER BY occurred_at DESC LIMIT ?";
inline constexpr std::string_view INSERT =
    "INSERT INTO event (event_type, severity, source, summary, details, "
    "occurred_at) "
    "VALUES (?, ?, ?, ?, ?, ?)";
inline constexpr std::string_view REMOVE =
    "UPDATE event SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view LINK_PERSON =
    "INSERT OR IGNORE INTO person_event "
    "(person_id, event_id, confidence) VALUES (?, ?, ?)";

inline constexpr std::string_view FIND_PERSONS_BY_EVENT =
    "SELECT * FROM person_event WHERE event_id = ?";

inline constexpr std::string_view SYNC_FIND =
    "SELECT * FROM event "
    "WHERE deleted_at IS NULL AND created_at >= ? AND created_at <= ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_FROM =
    "SELECT * FROM event "
    "WHERE deleted_at IS NULL AND created_at >= ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_AFTER =
    "SELECT * FROM event WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) AND created_at <= ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_AFTER_FROM =
    "SELECT * FROM event WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_ALL =
    "SELECT * FROM event WHERE deleted_at IS NULL "
    "ORDER BY created_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view SYNC_FIND_DELETED =
    "SELECT * FROM event "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? AND deleted_at <= ? "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_DELETED_FROM =
    "SELECT * FROM event "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_DELETED_AFTER =
    "SELECT * FROM event WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) AND deleted_at <= ? "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_DELETED_AFTER_FROM =
    "SELECT * FROM event WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_DELETED_ALL =
    "SELECT * FROM event WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view SYNC_FIND_LAST =
    "SELECT * FROM event WHERE deleted_at IS NULL "
    "ORDER BY created_at DESC LIMIT 1";
inline constexpr std::string_view SYNC_FIND_LAST_DELETED =
    "SELECT * FROM event WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at DESC LIMIT 1";

} // namespace event_query

struct EventCreateInput
{
  std::string eventType;
  EventSeverity severity{EventSeverity::Info};
  std::string source;
  std::string summary;
  std::string details;
  int64_t occurredAt{0};
};

struct EventLinkPersonInput
{
  int64_t eventId{0};
  int64_t personId{0};
  double confidence{0.0};
};
