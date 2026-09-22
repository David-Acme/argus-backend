#pragma once

#include "change-outbox-query.hxx"

#include <drogon/utils/coroutine.h>
#include <cstdint>
#include <optional>
#include <string>

// Durable outbox of camera-domain change events: the sink enqueues on the
// event loop and a drain thread publishes one pending row at a time, marking
// it sent only after the JetStream PubAck.
class ChangeOutboxRepository
{
public:
  ChangeOutboxRepository() = default;
  ~ChangeOutboxRepository() = default;

  [[nodiscard]] drogon::Task<ChangeOutboxDisposition> enqueue(
      const ChangeOutboxEnqueueInput& input) const;

  [[nodiscard]] std::optional<ChangeOutboxRow> nextPending() const;

  [[nodiscard]] bool markSent(const std::string& eventId, int64_t at) const;

  [[nodiscard]] bool recordAttempt(const std::string& eventId) const;
};
