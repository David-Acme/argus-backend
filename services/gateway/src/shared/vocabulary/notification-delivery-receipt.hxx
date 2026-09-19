#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Gateway-side delivery receipt lifecycle; the CHECK constraint mirrors it.
// Unknown persisted values fail closed at the call site: fromString returns
// nullopt instead of defaulting to a re-executable state.
enum class NotificationDeliveryReceipt : uint8_t
{
  Received = 0,
  Dispatched,
  Conflict,
  DeadLettered
};

inline std::string
notificationDeliveryReceiptToString(NotificationDeliveryReceipt status)
{
  switch (status) {
    case NotificationDeliveryReceipt::Received:
      return "received";
    case NotificationDeliveryReceipt::Dispatched:
      return "dispatched";
    case NotificationDeliveryReceipt::Conflict:
      return "conflict";
    case NotificationDeliveryReceipt::DeadLettered:
      return "dead_lettered";
  }
  return "received";
}

inline std::optional<NotificationDeliveryReceipt>
notificationDeliveryReceiptFromString(const std::string& value)
{
  if (value == "received")
    return NotificationDeliveryReceipt::Received;
  if (value == "dispatched")
    return NotificationDeliveryReceipt::Dispatched;
  if (value == "conflict")
    return NotificationDeliveryReceipt::Conflict;
  if (value == "dead_lettered")
    return NotificationDeliveryReceipt::DeadLettered;
  return std::nullopt;
}
