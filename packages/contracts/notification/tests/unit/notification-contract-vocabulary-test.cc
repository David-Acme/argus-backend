#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <notification/notification-delivery-status.hxx>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("notification delivery status strings round-trip")
{
  const std::vector<std::pair<NotificationDeliveryStatus, std::string>>
      pairs{{NotificationDeliveryStatus::Pending, "pending"},
            {NotificationDeliveryStatus::Sent, "sent"}};
  for (const auto& [status, name] : pairs) {
    CAPTURE(name);
    CHECK(notificationDeliveryStatusToString(status) == name);
    CHECK(notificationDeliveryStatusFromString(name) == status);
  }
}

TEST_CASE("anything the constraint would reject reads back as pending")
{
  CHECK(notificationDeliveryStatusFromString("Sent") ==
        NotificationDeliveryStatus::Pending);
  CHECK(notificationDeliveryStatusFromString("PENDING") ==
        NotificationDeliveryStatus::Pending);
  CHECK(notificationDeliveryStatusFromString("") ==
        NotificationDeliveryStatus::Pending);
  CHECK(notificationDeliveryStatusFromString("bogus") ==
        NotificationDeliveryStatus::Pending);
}

TEST_CASE("an unknown ordinal formats as pending rather than throwing")
{
  CHECK(notificationDeliveryStatusToString(
            static_cast<NotificationDeliveryStatus>(7)) == "pending");
  CHECK(notificationDeliveryStatusToString(
            static_cast<NotificationDeliveryStatus>(255)) == "pending");
}
