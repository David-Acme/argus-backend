#pragma once

#include "change-outbox-query.hxx"

#include <drogon/utils/coroutine.h>
#include <cstdint>
#include <string>
#include <vector>

// Durable outbox of camera-domain change events: the sink enqueues on the
// event loop and a drain thread walks the pending rows in one batch, marking
// each sent only after its own JetStream PubAck.
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
};
