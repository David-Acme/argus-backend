#pragma once

#include "pending-object-delete-query.hxx"

#include <drogon/utils/coroutine.h>

class PendingObjectDeleteRepository
{
public:
  drogon::Task<void> enqueue(const PendingObjectEnqueueInput& input) const;
  [[nodiscard]] drogon::Task<std::vector<PendingObjectDelete>>
  findDue(const PendingObjectDueInput& input) const;
  drogon::Task<void> remove(int64_t id) const;
  drogon::Task<void> postpone(const PendingObjectPostponeInput& input) const;
  [[nodiscard]] drogon::Task<int64_t> count() const;
};
