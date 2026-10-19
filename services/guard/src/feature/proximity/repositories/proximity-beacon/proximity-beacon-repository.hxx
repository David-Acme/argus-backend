#pragma once

#include "proximity-beacon-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <vector>

class ProximityBeaconRepository
{
public:
  [[nodiscard]] drogon::Task<std::optional<ProximityBeaconRow>> find(int64_t userId) const;
  [[nodiscard]] drogon::Task<std::vector<ProximityBeaconRow>> findEnabled() const;
  drogon::Task<void> upsert(const ProximityBeaconUpsertInput& input) const;
  drogon::Task<void> disable(int64_t userId, int64_t now) const;
};
