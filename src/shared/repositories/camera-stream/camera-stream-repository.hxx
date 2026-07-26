#pragma once

#include "camera-stream-query.hxx"
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <shared/contracts/syncable.hxx>
#include <shared/schemas/camera-stream/camera-stream-schema.hxx>
#include <vector>

class CameraStreamRepository : public Syncable
{
public:
  CameraStreamRepository() = default;
  ~CameraStreamRepository() override = default;

  drogon::Task<std::optional<CameraStreamSchema>>
  findById(int64_t id) const;
  drogon::Task<std::vector<CameraStreamSchema>>
  findByCamera(int64_t cameraId) const;
  drogon::Task<CameraStreamSchema>
  create(const CameraStreamCreateInput& input) const;
  drogon::Task<CameraStreamSchema>
  update(int64_t id, const CameraStreamUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>> findLast() const override;
  drogon::Task<std::optional<Json::Value>> findLastDeleted() const override;
};
