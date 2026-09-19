#pragma once

#include <shared/contracts/notification-delivery-sink.hxx>

#include <cstdint>
#include <string>
#include <string_view>

namespace delivery_inbox_query
{
// Receipts live in the gateway database: the insert wins the dispatch lease,
// a 'dispatched' row drops redeliveries, a 'received' row replays them.
inline constexpr std::string_view SELECT_RECEIPT =
    "SELECT status, fingerprint FROM notification_delivery_inbox "
    "WHERE delivery_id = ?";
inline constexpr std::string_view INSERT_RECEIPT =
    "INSERT OR IGNORE INTO notification_delivery_inbox "
    "(delivery_id, notification_id, user_id, fingerprint, attempts, status, "
    "created_at, updated_at) VALUES (?, ?, ?, ?, 0, 'received', ?, ?)";
inline constexpr std::string_view ADOPT_FINGERPRINT =
    "UPDATE notification_delivery_inbox SET fingerprint = ?, updated_at = ? "
    "WHERE delivery_id = ? AND status IN ('received', 'dispatched')";
inline constexpr std::string_view NOTE_ATTEMPT =
    "UPDATE notification_delivery_inbox SET attempts = attempts + 1, "
    "updated_at = ? WHERE delivery_id = ? RETURNING attempts";
inline constexpr std::string_view MARK_DISPATCHED =
    "UPDATE notification_delivery_inbox SET status = 'dispatched', "
    "updated_at = ? WHERE delivery_id = ? AND status = 'received'";
inline constexpr std::string_view MARK_DEAD_LETTERED =
    "UPDATE notification_delivery_inbox SET status = 'dead_lettered', "
    "updated_at = ? WHERE delivery_id = ? AND status = 'received'";
inline constexpr std::string_view MARK_CONFLICT =
    "UPDATE notification_delivery_inbox SET status = 'conflict', "
    "fingerprint = ?, updated_at = ? "
    "WHERE delivery_id = ? AND status IN ('received', 'dispatched')";
inline constexpr std::string_view FORCE_DEAD_LETTERED =
    "UPDATE notification_delivery_inbox SET status = 'dead_lettered', "
    "updated_at = ? WHERE delivery_id = ?";
} // namespace delivery_inbox_query

struct DeliveryReceiptInput
{
  NotificationDeliveryEvent event;
  int64_t at{0};
};

struct DeliveryReceipt
{
  bool duplicate{false};
};
