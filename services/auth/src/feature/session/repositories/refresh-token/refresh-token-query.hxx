#pragma once

#include <string_view>

namespace refresh_token_query
{

inline constexpr std::string_view FIND_BY_ACCESS_TOKEN =
    "SELECT * FROM refresh_token "
    "WHERE user_id = ? AND access_token = ? AND is_valid = 1 AND is_used = 0";

inline constexpr std::string_view INVALIDATE_ALL_USER =
    "UPDATE refresh_token SET is_valid = 0 "
    "WHERE user_id = ? AND is_valid = 1";

}
