#pragma once

#include "camera-patrol-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/camera-control/schemas/camera-patrol/camera-patrol-schema.hxx>
#include <optional>
#include <vector>

class CameraPatrolRepository
{
public:
  drogon::Task<std::optional<CameraPatrolSchema>>
  findByCamera(int64_t cameraId, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<std::vector<ActiveCameraPatrol>> findActive() const;
  drogon::Task<CameraPatrolSchema> create(const CameraPatrolCreateInput& input) const;
  drogon::Task<std::optional<CameraPatrolSchema>> update(int64_t cameraId,
                                                         const CameraPatrolUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t cameraId, drogon::orm::DbClient* client = nullptr) const;
};
