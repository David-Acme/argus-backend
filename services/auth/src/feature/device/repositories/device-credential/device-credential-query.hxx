#pragma once

#include <string_view>

namespace device_credential_query
{

inline constexpr std::string_view FIND_ACTIVE_BY_SECRET_HASH =
    "SELECT * FROM device_credential WHERE secret_hash = ? AND is_active = 1";

}
