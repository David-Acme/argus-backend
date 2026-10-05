#pragma once

#include "call-query.hxx"

#include <drogon/utils/coroutine.h>
#include <feature/call/schemas/call/call-schema.hxx>

#include <optional>
#include <vector>

class CallRepository
{
public:
  CallRepository() = default;

  drogon::Task<std::optional<int64_t>> create(const CallCreateInput& input) const;

  drogon::Task<std::optional<int64_t>>
  createRinging(const CallCreateInput& input) const;

  drogon::Task<bool> exists(const std::string& dedupeKey, int64_t userId) const;

  drogon::Task<std::optional<CallSchema>> findById(int64_t id) const;

  drogon::Task<std::optional<CallSchema>>
  findRingingFor(const CallRingingInput& input) const;

  drogon::Task<CallRingStats> ringStats(const CallRingStatsInput& input) const;

  drogon::Task<bool> claim(const CallClaimInput& input) const;

  drogon::Task<bool> markMissed(int64_t id, int64_t now) const;

  drogon::Task<bool> end(const CallEndInput& input) const;

  drogon::Task<bool> markPushed(int64_t id, int64_t now) const;

  drogon::Task<std::vector<CallSchema>> findFollowups(int64_t parentId) const;

  drogon::Task<int64_t> settleFollowups(const CallSettleInput& input) const;

  drogon::Task<std::vector<CallSchema>> findRinging() const;

  drogon::Task<std::vector<CallCancelled>>
  cancelRingingForKey(const CallCancelForKeyInput& input) const;

  drogon::Task<int64_t> closeStaleAnswered(int64_t answeredBefore,
                                           int64_t now) const;
};
