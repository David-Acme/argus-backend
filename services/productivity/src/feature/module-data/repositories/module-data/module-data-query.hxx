#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace module_data_query
{

inline constexpr std::string_view SUMMARY =
    "SELECT "
    "(SELECT COUNT(*) FROM project WHERE deleted_at IS NULL) AS projects, "
    "(SELECT COUNT(*) FROM project_task WHERE deleted_at IS NULL) AS tasks, "
    "(SELECT COUNT(*) FROM project_member WHERE deleted_at IS NULL) AS members, "
    "(SELECT COUNT(*) FROM calendar_event WHERE deleted_at IS NULL) AS events, "
    "(SELECT COUNT(*) FROM calendar_event_share WHERE deleted_at IS NULL) AS shares, "
    "(SELECT COALESCE(SUM(?1 + length(name) + length(description) + length(color)), 0) FROM project) + "
    "(SELECT COALESCE(SUM(?1 + length(title)), 0) FROM project_task) + "
    "(SELECT COUNT(*) * ?1 FROM project_member) + "
    "(SELECT COALESCE(SUM(?1 + length(title) + length(description) + length(location) + "
    "length(color) + COALESCE(length(recurrence_rule), 0)), 0) FROM calendar_event) + "
    "(SELECT COUNT(*) * ?1 FROM calendar_event_share) AS bytes";

inline constexpr std::array<std::string_view, 8> PURGE{
    "DELETE FROM calendar_event_share",
    "DELETE FROM calendar_event",
    "DELETE FROM project_member",
    "DELETE FROM project_task",
    "DELETE FROM project",
    "DELETE FROM agenda_notice WHERE kind = 'event'",
    "DELETE FROM idempotency_key WHERE route IN ('project', 'project_task', 'project_member', "
    "'calendar_event', 'calendar_event_share')",
    "DELETE FROM change_outbox"};

inline constexpr int64_t kRowOverheadBytes = 64;

}
