#pragma once

#include <cstdint>
#include <string>

// Change-feed outbox lifecycle; the CHECK constraint mirrors it.
enum class ChangeOutboxStatus : uint8_t
{
  Pending = 0,
  Sent
};

[[nodiscard]] inline std::string
changeOutboxStatusToString(ChangeOutboxStatus status)
{
  switch (status) {
    case ChangeOutboxStatus::Pending:
      return "pending";
    case ChangeOutboxStatus::Sent:
      return "sent";
  }
  return "pending";
}

[[nodiscard]] inline ChangeOutboxStatus
changeOutboxStatusFromString(const std::string& value)
{
  if (value == "sent")
    return ChangeOutboxStatus::Sent;
  return ChangeOutboxStatus::Pending;
}
