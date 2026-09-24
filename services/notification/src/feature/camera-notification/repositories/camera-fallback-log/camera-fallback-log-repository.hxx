#pragma once

#include "camera-fallback-log-query.hxx"

#include <drogon/utils/coroutine.h>

class CameraFallbackLogRepository
{
public:
  CameraFallbackLogRepository() = default;
  ~CameraFallbackLogRepository() = default;

  drogon::Task<bool> log(const CameraFallbackLogInput& input) const;

  drogon::Task<int64_t> purgeOlderThan(int64_t olderThan) const;
};
