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

  [[nodiscard]] drogon::Task<ChangeOutboxDisposition> enqueue(
      const ChangeOutboxEnqueueInput& input) const;

  [[nodiscard]] std::vector<ChangeOutboxRow> pendingBatch(int limit) const;

  [[nodiscard]] bool markSent(const std::string& eventId, int64_t at) const;

  [[nodiscard]] bool recordAttempt(const std::string& eventId) const;

  [[nodiscard]] int64_t purgeSent(int64_t olderThanMs) const;
};
