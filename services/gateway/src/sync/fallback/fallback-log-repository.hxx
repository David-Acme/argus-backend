#pragma once

#include "fallback-log-query.hxx"

#include <drogon/utils/coroutine.h>

class FallbackLogRepository
{
public:
  FallbackLogRepository() = default;
  ~FallbackLogRepository() = default;

  drogon::Task<bool> log(const FallbackLogInput& input) const;

  drogon::Task<int64_t> purgeOlderThan(int64_t olderThan) const;
};
