#pragma once

#include "change-outbox-query.hxx"

#include <drogon/utils/coroutine.h>
#include <cstdint>
#include <string>
#include <vector>

class ChangeOutboxRepository
{
public:
  ChangeOutboxRepository() = default;
  ~ChangeOutboxRepository() = default;

  [[nodiscard]] drogon::Task<ChangeOutboxDisposition>
  enqueue(const ChangeOutboxEnqueueInput& input) const;

  [[nodiscard]] drogon::Task<bool>
  enqueueAction(const ChangeOutboxActionInput& input) const;

  [[nodiscard]] std::vector<ChangeOutboxRow> pendingBatch(int limit) const;

  [[nodiscard]] bool markSent(int64_t id, int64_t at) const;

  [[nodiscard]] bool recordAttempt(int64_t id) const;
};
