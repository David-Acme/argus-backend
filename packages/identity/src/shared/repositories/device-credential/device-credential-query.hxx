#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace device_credential_query
{

inline constexpr std::string_view FIND_ACTIVE_BY_SECRET_HASH =
    "SELECT * FROM device_credential WHERE secret_hash = ? AND is_active = 1";

inline constexpr std::string_view INSERT =
    "INSERT INTO device_credential (user_id, device_hash, secret_hash) "
    "VALUES (?, ?, ?)";

} // namespace device_credential_query

struct DeviceCredentialCreateInput
{
  int64_t userId{0};
  std::string deviceHash;
  std::string secretHash;
};
