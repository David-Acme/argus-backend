#pragma once

#include "change-outbox-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <string>
#include <vector>

class ChangeOutboxRepository
{
public:
  ChangeOutboxRepository() = default;
  ~ChangeOutboxRepository() = default;

  [[nodiscard]] drogon::Task<void>
  enqueueAction(const ChangeOutboxActionInput& input) const;

  [[nodiscard]] std::vector<ChangeOutboxRow> pendingBatch(int limit) const;

  [[nodiscard]] bool markSent(int64_t id, int64_t at) const;

  [[nodiscard]] bool recordAttempt(int64_t id) const;

  [[nodiscard]] int64_t purgeSent(int64_t olderThanMs) const;
};
