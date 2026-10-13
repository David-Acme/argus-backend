#pragma once

#include "pin-attempt-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>

class PinAttemptRepository
{
public:
  [[nodiscard]] drogon::Task<int64_t> failuresSince(PinAttemptWindowInput input) const;

  drogon::Task<void> recordFailure(PinFailureInput input) const;

  drogon::Task<void> clear(int64_t userId) const;

  [[nodiscard]] drogon::Task<int64_t> purgeBefore(int64_t cutoff) const;
};
