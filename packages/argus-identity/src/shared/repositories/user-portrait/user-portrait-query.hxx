#pragma once

#include <cstdint>
#include <string_view>

namespace user_portrait_query
{

inline constexpr std::string_view FIND_BY_USER_ID =
    "SELECT * FROM user_portrait WHERE user_id = ?";

inline constexpr std::string_view UPSERT_CURRENT =
    "INSERT INTO user_portrait (user_id, file_id) VALUES (?, ?) "
    "ON CONFLICT(user_id) DO UPDATE SET file_id = excluded.file_id, "
    "updated_at = strftime('%s', 'now')";

} // namespace user_portrait_query

struct UserPortraitUpsertInput
{
  int64_t userId{0};
  int64_t fileId{0};
};
