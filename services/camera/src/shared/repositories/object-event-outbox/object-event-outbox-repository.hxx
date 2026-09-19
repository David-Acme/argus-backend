#pragma once

#include "object-event-outbox-query.hxx"

#include <cstdint>
#include <optional>
#include <string>

// Durable single-writer outbox for object observations: the event row and the
// class cooldown advance commit in one transaction, and a row only leaves
// 'pending' after the broker PubAck. Sync API: the operator runs off the loop.
class ObjectEventOutboxRepository
{
public:
  ObjectEventOutboxRepository() = default;
  ~ObjectEventOutboxRepository() = default;

  ObjectEventEnqueueOutcome enqueue(
      const ObjectEventEnqueueInput& input) const;

  std::optional<ObjectEventRow> nextPending() const;

  bool markSent(const std::string& eventId, int64_t at) const;

  bool recordAttempt(const std::string& eventId) const;

  ObjectEventOutboxStats stats() const;

  // Drops cooldown rows older than the retained window; active rows survive.
  int64_t purgeExpiredCooldowns(int64_t olderThanMs) const;
};
