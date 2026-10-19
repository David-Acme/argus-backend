#pragma once

#include "proximity-sensor-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <vector>

class ProximitySensorRepository
{
public:
  [[nodiscard]] drogon::Task<std::vector<ProximitySensorRow>> find() const;
  [[nodiscard]] drogon::Task<ProximitySensorRow> create(const ProximitySensorCreateInput& input) const;
  [[nodiscard]] drogon::Task<bool> remove(int64_t id) const;
};
