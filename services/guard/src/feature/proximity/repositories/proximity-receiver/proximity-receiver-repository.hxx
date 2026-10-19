#pragma once

#include "proximity-receiver-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <vector>

class ProximityReceiverRepository
{
public:
  [[nodiscard]] drogon::Task<std::vector<ProximityReceiverRow>> find() const;
  [[nodiscard]] drogon::Task<int64_t> count() const;
  [[nodiscard]] drogon::Task<ProximityReceiverRow> create(const ProximityReceiverCreateInput& input) const;
  [[nodiscard]] drogon::Task<bool> remove(int64_t id) const;
};
