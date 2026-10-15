#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>

namespace push_delivery_retention
{
inline constexpr int64_t kMaxAgeNs = int64_t{24} * 60 * 60 * 1000000000;
}

struct PushDeliveryRecord
{
  std::string eventId;
  std::string payload;
  drogon::orm::DbClient* client{nullptr};
};

class PushDeliveryOutbox
{
public:
  virtual ~PushDeliveryOutbox() = default;

  [[nodiscard]] virtual drogon::Task<void>
  record(PushDeliveryRecord entry) const = 0;
};
