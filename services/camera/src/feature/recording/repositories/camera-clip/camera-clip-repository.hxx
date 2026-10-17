#pragma once

#include "camera-clip-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <feature/recording/schemas/camera-clip/camera-clip-schema.hxx>
#include <vector>

class CameraClipRepository
{
public:
  drogon::Task<CameraClipSchema> create(const CameraClipCreateInput& input) const;
  drogon::Task<std::optional<CameraClipSchema>>
  findById(int64_t id, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<std::vector<CameraClipSchema>>
  findByCamera(const CameraClipPageInput& input) const;
  drogon::Task<std::vector<CameraClipSchema>>
  findByCameraAfter(const CameraClipAfterInput& input) const;
  drogon::Task<bool> markDeleted(const CameraClipDeleteInput& input) const;
  drogon::Task<std::vector<CameraClipSchema>>
  findExpired(const CameraClipSweepInput& input) const;
  drogon::Task<int64_t> countExpired(int64_t now, int64_t maxRetentionDays) const;
  drogon::Task<std::optional<int64_t>>
  retentionDaysOf(int64_t cameraId, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<void> purgeDeleted(int64_t before) const;
};
