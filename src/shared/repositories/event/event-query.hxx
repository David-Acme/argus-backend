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

