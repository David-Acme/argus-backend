#pragma once

#include <cstdint>
#include <string>

namespace voice_device_query
{
inline constexpr const char* RECORD =
    "INSERT INTO voice_device (device_hash, user_id, calls, matched, "
    "conflicting, mixed, last_call_at) VALUES (?, ?, 1, ?, ?, ?, ?) "
    "ON CONFLICT(device_hash, user_id) DO UPDATE SET calls = calls + 1, "
    "matched = matched + excluded.matched, "
    "conflicting = conflicting + excluded.conflicting, "
    "mixed = mixed + excluded.mixed, last_call_at = excluded.last_call_at "
    "RETURNING *";

inline constexpr const char* SHARED_FOR_USER =
    "SELECT d.device_hash FROM voice_device d WHERE d.user_id = ? AND "
    "(SELECT COUNT(*) FROM voice_device o WHERE o.device_hash = d.device_hash) "
    "> 1";

inline constexpr const char* OTHER_USERS_ON_DEVICE =
    "SELECT COUNT(*) AS total FROM voice_device WHERE device_hash = ? "
    "AND user_id <> ?";

inline constexpr const char* DELETE_BY_USER =
    "DELETE FROM voice_device WHERE user_id = ?";
}

struct VoiceDeviceRecordInput
{
  std::string deviceHash;
  int64_t userId{0};
  int matched{0};
  int conflicting{0};
  int mixed{0};
  int64_t now{0};
};
