#pragma once

#include "crop-capability-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>

class CropCapabilityRepository
{
public:
  drogon::Task<void> create(const CropCapabilityCreateInput& input) const;
  [[nodiscard]] drogon::Task<std::optional<ConsumedCropCapability>>
  consume(const CropCapabilityConsumeInput& input) const;
  drogon::Task<void> purgeExpired(int64_t now) const;
};
