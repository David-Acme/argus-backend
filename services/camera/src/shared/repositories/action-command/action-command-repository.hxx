#pragma once

#include "action-command-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>
#include <vector>

// Command idempotency inbox and siren lease for the guard action surface.
class ActionCommandRepository
{
public:
  ActionCommandRepository() = default;
  ~ActionCommandRepository() = default;

  // Boot-only additive migration: adds the post-Round-5 columns and remaps
  // legacy states fail-closed. Safe to call repeatedly.
  bool migrateLegacySchema() const;

  drogon::Task<ActionClaim> claim(const ActionCommandClaimInput& input) const;

  // Authoritative durable row for a command id; used when a settle loses its
  // generation fence.
  drogon::Task<ActionCommandRow> find(const std::string& commandId) const;

  drogon::Task<bool> settle(const ActionCommandResultInput& input) const;

  // Boot-only: expired in-flight claims become retryable.
  drogon::Task<int64_t> reconcileExpired(int64_t at,
                                         int64_t leaseSeconds) const;

  drogon::Task<bool> upsertLease(const SirenLeaseInput& input) const;

  drogon::Task<bool> deleteLease(int64_t cameraId) const;

  drogon::Task<std::vector<int64_t>> expiredLeases(int64_t now) const;
};
