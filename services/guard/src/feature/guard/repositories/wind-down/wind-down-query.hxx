#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string_view>

namespace wind_down_query
{
inline constexpr std::string_view DROP_OBSERVATIONS =
    "UPDATE guard_observation_inbox SET status = 'completed', completed_at = ?, "
    "updated_at = ? WHERE status = 'processing'";

inline constexpr std::string_view DROP_ACTIONS =
    "UPDATE guard_action_outbox SET status = 'rejected', detail = ?, updated_at = ? "
    "WHERE status IN ('pending', 'in_flight', 'retryable_failed')";

inline constexpr std::string_view END_DUTY =
    "UPDATE guard_response_recipient SET on_duty = 0, updated_at = ? WHERE on_duty = 1";

inline constexpr std::string_view COUNT_OBSERVATIONS =
    "SELECT COUNT(*) AS total FROM guard_observation_inbox WHERE status = 'processing'";

inline constexpr std::string_view COUNT_ACTIONS =
    "SELECT COUNT(*) AS total FROM guard_action_outbox "
    "WHERE status IN ('pending', 'in_flight', 'retryable_failed')";

inline constexpr std::string_view COUNT_DUTY =
    "SELECT COUNT(*) AS total FROM guard_response_recipient WHERE on_duty = 1";

inline constexpr std::string_view kDropReason = "module_disabled";
}

struct WindDownInput
{
  int64_t at{0};
  drogon::orm::DbClient* client{nullptr};
};

struct WindDownReport
{
  int64_t observations{0};
  int64_t actions{0};
  int64_t duty{0};

  [[nodiscard]] bool empty() const { return observations == 0 && actions == 0 && duty == 0; }
};
