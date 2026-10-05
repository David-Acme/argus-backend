#pragma once

#include "arrival-seen-query.hxx"

#include <drogon/utils/coroutine.h>

class ArrivalSeenRepository
{
public:
  ArrivalSeenRepository() = default;

  drogon::Task<int64_t> lastSeen(int64_t personId) const;

  drogon::Task<void> touch(const ArrivalSeenInput& input) const;

  drogon::Task<int64_t> purgeStale(int64_t seenBefore) const;
};
