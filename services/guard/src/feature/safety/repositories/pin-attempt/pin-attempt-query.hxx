#pragma once

#include <cstdint>
#include <string_view>

namespace pin_attempt_query
{

inline constexpr std::string_view PRUNE_USER =
    "DELETE FROM guard_pin_attempt WHERE user_id = ? AND failed_at <= ?";

inline constexpr std::string_view COUNT_SINCE =
    "SELECT COUNT(*) AS total FROM guard_pin_attempt WHERE user_id = ? AND failed_at > ?";

inline constexpr std::string_view INSERT_FAILURE =
    "INSERT INTO guard_pin_attempt (user_id, failed_at) VALUES (?, ?)";

inline constexpr std::string_view CLEAR_USER = "DELETE FROM guard_pin_attempt WHERE user_id = ?";

inline constexpr std::string_view PURGE_BEFORE = "DELETE FROM guard_pin_attempt WHERE failed_at <= ?";

}

struct PinAttemptWindowInput
{
  int64_t userId{0};
  int64_t since{0};
};

struct PinFailureInput
{
  int64_t userId{0};
  int64_t at{0};
};
