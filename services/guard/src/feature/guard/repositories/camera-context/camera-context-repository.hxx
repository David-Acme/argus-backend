#pragma once

#include "camera-context-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <vector>

class CameraContextRepository
{
public:
  [[nodiscard]] drogon::Task<GuardCameraContext> find(int64_t cameraId) const;

  [[nodiscard]] drogon::Task<std::vector<GuardCameraContext>> list() const;

  [[nodiscard]] drogon::Task<GuardCameraContext>
  upsert(const CameraContextUpsertInput& input) const;
};
