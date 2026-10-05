#pragma once

#include "arrival-seen-query.hxx"

#include <drogon/utils/coroutine.h>

class ArrivalSeenRepository
{
public:
  ArrivalSeenRepository() = default;

  drogon::Task<int64_t> touch(const ArrivalSeenInput& input) const;

  drogon::Task<int64_t> purgeStale(int64_t seenBefore) const;
};
