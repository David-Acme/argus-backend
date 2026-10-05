#pragma once

#include "action-command-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>
#include <vector>

class ActionCommandRepository
{
public:
  ActionCommandRepository() = default;
  ~ActionCommandRepository() = default;

  bool migrateLegacySchema() const;

  drogon::Task<ActionClaim> claim(const ActionCommandClaimInput& input) const;

  drogon::Task<ActionCommandRow> find(const std::string& commandId) const;

  drogon::Task<bool> settle(const ActionCommandResultInput& input) const;

  drogon::Task<int64_t> reconcileExpired(int64_t at,
                                         int64_t leaseSeconds) const;

  drogon::Task<int64_t> purgeSettled(int64_t olderThan) const;

  drogon::Task<bool> upsertLease(const SirenLeaseInput& input) const;

  drogon::Task<bool> deleteLease(int64_t cameraId) const;

  drogon::Task<std::vector<int64_t>> expiredLeases(int64_t now) const;
};
