#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <notification/notification-delivery-status.hxx>
#include <string>
#include <utility>
#include <vector>

// The two spellings the delivery column accepts, and nothing else: the schema
// stores TEXT with CHECK (status IN ('pending', 'sent')), so the spelling is
// the contract and the ordinals behind it are never stored or sent. The
// round-trip below is therefore the whole vocabulary.
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
  // The fallback is the whole error policy: a caller that hands over a
  // spelling the column cannot store gets the initial state rather than a
  // throw, so a bad value can never be written back as sent.
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
  // Not the errors vocabulary's policy, whose toString throws on a code it
  // does not know: this one formats a status it cannot name as the initial
  // one, so a row read back by a newer schema version still renders.
  CHECK(notificationDeliveryStatusToString(
            static_cast<NotificationDeliveryStatus>(7)) == "pending");
  CHECK(notificationDeliveryStatusToString(
            static_cast<NotificationDeliveryStatus>(255)) == "pending");
}
