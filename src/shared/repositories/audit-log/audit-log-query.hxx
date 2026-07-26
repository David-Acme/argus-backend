#pragma once
#include <string_view>

namespace audit_log_query
{
inline constexpr std::string_view FIND_BY_RECORD =
    "SELECT * FROM audit_log WHERE record_id = ? AND table_name = ? "
    "ORDER BY event_timestamp DESC LIMIT ?";
inline constexpr std::string_view FIND_BY_TABLE =
    "SELECT * FROM audit_log WHERE table_name = ? "
    "ORDER BY event_timestamp DESC LIMIT ?";
} // namespace audit_log_query
