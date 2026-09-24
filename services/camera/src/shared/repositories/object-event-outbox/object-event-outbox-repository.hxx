#pragma once

#include "object-event-outbox-query.hxx"

#include <cstdint>
#include <string>
#include <vector>

class ObjectEventOutboxRepository
{
public:
  ObjectEventOutboxRepository() = default;
  ~ObjectEventOutboxRepository() = default;

  ObjectEventEnqueueOutcome enqueue(
      const ObjectEventEnqueueInput& input) const;

  [[nodiscard]] std::vector<ObjectEventRow> pendingBatch(int limit) const;

  bool markSent(const std::string& eventId, int64_t at) const;

  bool recordAttempt(const std::string& eventId) const;

  ObjectEventOutboxStats stats() const;

  int64_t purgeExpiredCooldowns(int64_t olderThanMs) const;

  [[nodiscard]] int64_t purgeSettled(int64_t olderThanMs) const;
};
