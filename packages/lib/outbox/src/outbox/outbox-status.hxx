#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace outbox
{

enum class OutboxStatus : uint8_t
{
  Pending = 0,
  Sent
};

[[nodiscard]] inline std::string outboxStatusToString(OutboxStatus status)
{
  switch (status) {
    case OutboxStatus::Pending:
      return "pending";
    case OutboxStatus::Sent:
      return "sent";
  }
  return "pending";
}

[[nodiscard]] inline OutboxStatus outboxStatusFromString(std::string_view value)
{
  if (value == "sent")
    return OutboxStatus::Sent;
  return OutboxStatus::Pending;
}

}
