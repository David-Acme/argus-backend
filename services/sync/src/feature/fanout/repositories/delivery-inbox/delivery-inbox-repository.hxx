#pragma once

#include "delivery-inbox-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>

class DeliveryInboxRepository
{
public:
  DeliveryInboxRepository() = default;

  [[nodiscard]] drogon::Task<DeliveryReceipt> receive(
      const DeliveryReceiptInput& input) const;

  [[nodiscard]] drogon::Task<int64_t> noteAttempt(int64_t deliveryId,
                                                  int64_t at) const;

  [[nodiscard]] drogon::Task<bool> markDispatched(int64_t deliveryId,
                                                  int64_t at) const;

  [[nodiscard]] drogon::Task<bool> markDeadLettered(int64_t deliveryId,
                                                    int64_t at) const;
};
