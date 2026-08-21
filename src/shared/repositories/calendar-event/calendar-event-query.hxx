#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace calendar_event_query
{
/** `?` the ownership predicate spends in every sync query of this table. */
inline constexpr int OWNERSHIP_PLACEHOLDERS = 2;


inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM calendar_event WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view FIND =
    "SELECT * FROM calendar_event "
    "WHERE deleted_at IS NULL AND created_at >= ? AND created_at <= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM calendar_event "
    "WHERE deleted_at IS NULL AND created_at >= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER =
    "SELECT * FROM calendar_event WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) AND created_at <= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER_FROM =
    "SELECT * FROM calendar_event WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM calendar_event "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? AND deleted_at <= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM calendar_event "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER =
    "SELECT * FROM calendar_event WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) AND deleted_at <= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER_FROM =
    "SELECT * FROM calendar_event WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM calendar_event WHERE deleted_at IS NULL "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM calendar_event WHERE deleted_at IS NOT NULL "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM calendar_event WHERE deleted_at IS NULL "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY created_at DESC LIMIT 1";
inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM calendar_event WHERE deleted_at IS NOT NULL "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM calendar_event_share s "
    "WHERE s.calendar_event_id = calendar_event.id AND s.user_id = ? "
    "AND s.deleted_at IS NULL)) "
    "ORDER BY deleted_at DESC LIMIT 1";

inline constexpr std::string_view FIND_BY_OWNER_RANGE =
    "SELECT * FROM calendar_event WHERE owner_id = ? AND deleted_at IS NULL "
    "AND starts_at >= ? AND starts_at <= ? "
    "ORDER BY starts_at ASC";

inline constexpr std::string_view INSERT =
    "INSERT INTO calendar_event (created_by, owner_id, project_id, title, description, location, color, starts_at, ends_at, is_all_day, recurrence_rule) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view UPDATE_PREFIX = "UPDATE calendar_event SET ";
inline constexpr std::string_view UPDATE_COL_TITLE = "title = ?";
inline constexpr std::string_view UPDATE_COL_DESCRIPTION = "description = ?";
inline constexpr std::string_view UPDATE_COL_LOCATION = "location = ?";
inline constexpr std::string_view UPDATE_COL_COLOR = "color = ?";
inline constexpr std::string_view UPDATE_COL_STARTS_AT = "starts_at = ?";
inline constexpr std::string_view UPDATE_COL_ENDS_AT = "ends_at = ?";
inline constexpr std::string_view UPDATE_COL_IS_ALL_DAY = "is_all_day = ?";
inline constexpr std::string_view UPDATE_COL_RECURRENCE_RULE = "recurrence_rule = ?";
inline constexpr std::string_view UPDATE_COL_PROJECT_ID = "project_id = ?";
inline constexpr std::string_view UPDATE_SUFFIX =
    ", updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view REMOVE =
    "UPDATE calendar_event SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

} // namespace calendar_event_query

struct CalendarEventCreateInput
{
  std::optional<int64_t> createdBy;
  int64_t ownerId{0};
  std::optional<int64_t> projectId;
  std::string title;
  std::string description;
  std::string location;
  std::string color;
  int64_t startsAt{0};
  std::optional<int64_t> endsAt;
  bool isAllDay{false};
  std::optional<std::string> recurrenceRule;
};

struct CalendarEventUpdateInput
{
  std::optional<std::string> title;
  std::optional<std::string> description;
  std::optional<std::string> location;
  std::optional<std::string> color;
  std::optional<int64_t> startsAt;
  std::optional<int64_t> endsAt;
  std::optional<bool> isAllDay;
  std::optional<std::string> recurrenceRule;
  std::optional<int64_t> projectId;
};
