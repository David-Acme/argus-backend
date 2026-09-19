#pragma once

#include <cstdint>
#include <string>

// Notification delivery intent lifecycle; the CHECK constraint mirrors it.
enum class NotificationDeliveryStatus : uint8_t
{
  Pending = 0,
  Sent
};

inline std::string
notificationDeliveryStatusToString(NotificationDeliveryStatus status)
{
  switch (status) {
    case NotificationDeliveryStatus::Pending:
      return "pending";
    case NotificationDeliveryStatus::Sent:
      return "sent";
  }
  return "pending";
}

inline NotificationDeliveryStatus
notificationDeliveryStatusFromString(const std::string& value)
{
  return value == "sent" ? NotificationDeliveryStatus::Sent
                         : NotificationDeliveryStatus::Pending;
}
