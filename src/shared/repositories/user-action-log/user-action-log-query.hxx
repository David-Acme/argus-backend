#pragma once
#include <string_view>

namespace user_action_log_query
{
inline constexpr std::string_view FIND_BY_USER =
    "SELECT * FROM user_action_log WHERE user_id = ? "
    "ORDER BY created_at DESC LIMIT ?";
inline constexpr std::string_view FIND_BY_RECORD =
    "SELECT * FROM user_action_log WHERE record_id = ? AND table_name = ? "
    "ORDER BY created_at DESC LIMIT ?";
} // namespace user_action_log_query
