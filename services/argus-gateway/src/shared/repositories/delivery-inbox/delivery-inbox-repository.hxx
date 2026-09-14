#pragma once

#include "delivery-inbox-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>

// Gateway-side durable inbox for argus.notification.v1.delivery, stored in
// the gateway database. Same delivery id plus same fingerprint is a replay;
// same id plus a different fingerprint is a conflict that is never
// dispatched. Unknown persisted statuses fail closed into 'dead_lettered'.
class DeliveryInboxRepository
{
public:
  DeliveryInboxRepository() = default;

  drogon::Task<DeliveryReceipt> receive(
      const DeliveryReceiptInput& input) const;

  drogon::Task<int64_t> noteAttempt(int64_t deliveryId, int64_t at) const;

  drogon::Task<bool> markDispatched(int64_t deliveryId, int64_t at) const;

  drogon::Task<bool> markDeadLettered(int64_t deliveryId, int64_t at) const;
};
