#include "delivery-inbox-repository.hxx"

#include <shared/enums.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/utils/sha256/sha256.hxx>
#include <string>
#include <trantor/utils/Logger.h>

using namespace delivery_inbox_query;

namespace
{
// Canonical payload identity for one delivery event.
std::string deliveryFingerprint(const NotificationDeliveryEvent& event)
{
  return argus::hash::sha256Hex(json_util::toString(event.toJson()));
}
} // namespace

drogon::Task<DeliveryReceipt> DeliveryInboxRepository::receive(
    const DeliveryReceiptInput& input) const
{
  DeliveryReceipt receipt{.duplicate = true};
  if (input.event.deliveryId <= 0)
    co_return receipt;
  const std::string fingerprint = deliveryFingerprint(input.event);
  auto client = DbService::client();
  const auto existing = co_await client->execSqlCoro(
      std::string(SELECT_RECEIPT), input.event.deliveryId);
  if (!existing.empty()) {
    const auto status = notificationDeliveryReceiptFromString(
        existing.front()["status"].as<std::string>());
    if (!status.has_value()) {
      LOG_ERROR << "Delivery inbox: unknown persisted status for delivery "
                << input.event.deliveryId << "; dead-lettering closed";
      co_await client->execSqlCoro(std::string(FORCE_DEAD_LETTERED), input.at,
                                   input.event.deliveryId);
      co_return receipt;
    }
    if (*status != NotificationDeliveryReceipt::Received) {
      if (*status == NotificationDeliveryReceipt::Dispatched) {
        const std::string stored =
            existing.front()["fingerprint"].as<std::string>();
        if (!stored.empty() && stored != fingerprint) {
          LOG_ERROR << "Delivery inbox: fingerprint conflict for delivery "
                    << input.event.deliveryId << "; never dispatched again";
          co_await client->execSqlCoro(std::string(MARK_CONFLICT), fingerprint,
                                       input.at, input.event.deliveryId);
        }
        else if (stored.empty()) {
          co_await client->execSqlCoro(std::string(ADOPT_FINGERPRINT),
                                       fingerprint, input.at,
                                       input.event.deliveryId);
        }
      }
      co_return receipt;
    }
    const std::string stored =
        existing.front()["fingerprint"].as<std::string>();
    if (stored.empty()) {
      co_await client->execSqlCoro(std::string(ADOPT_FINGERPRINT), fingerprint,
                                   input.at, input.event.deliveryId);
      receipt.duplicate = false;
      co_return receipt;
    }
    if (stored == fingerprint) {
      receipt.duplicate = false;
      co_return receipt;
    }
    LOG_ERROR << "Delivery inbox: fingerprint conflict for delivery "
              << input.event.deliveryId << "; never dispatched";
    co_await client->execSqlCoro(std::string(MARK_CONFLICT), fingerprint,
                                 input.at, input.event.deliveryId);
    co_return receipt;
  }
  const auto inserted = co_await client->execSqlCoro(
      std::string(INSERT_RECEIPT), input.event.deliveryId,
      input.event.notificationId, input.event.userId, fingerprint, input.at,
      input.at);
  receipt.duplicate = inserted.affectedRows() == 0;
  co_return receipt;
}

drogon::Task<int64_t> DeliveryInboxRepository::noteAttempt(
    int64_t deliveryId, int64_t at) const
{
  if (deliveryId <= 0)
    co_return -1;
  auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(std::string(NOTE_ATTEMPT), at,
                                                 deliveryId);
  if (rows.empty())
    co_return -1;
  co_return rows.front()["attempts"].as<int64_t>();
}

drogon::Task<bool> DeliveryInboxRepository::markDispatched(
    int64_t deliveryId, int64_t at) const
{
  if (deliveryId <= 0)
    co_return false;
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(std::string(MARK_DISPATCHED),
                                                   at, deliveryId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> DeliveryInboxRepository::markDeadLettered(
    int64_t deliveryId, int64_t at) const
{
  if (deliveryId <= 0)
    co_return false;
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(MARK_DEAD_LETTERED), at, deliveryId);
  co_return result.affectedRows() > 0;
}
