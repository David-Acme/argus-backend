#pragma once

#include "arrival-seen-query.hxx"

#include <drogon/utils/coroutine.h>

class ArrivalSeenRepository
{
public:
  ArrivalSeenRepository() = default;

  drogon::Task<int64_t> touch(const ArrivalSeenInput& input) const;
};
