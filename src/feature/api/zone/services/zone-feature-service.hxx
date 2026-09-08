#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/zone/dtos/create-zone-dto.hxx>
#include <feature/api/zone/dtos/update-zone-dto.hxx>
#include <optional>
#include <shared/contracts/camera-change-sink.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/repositories/zone/zone-repository.hxx>
#include <shared/schemas/zone/zone-schema.hxx>

class ZoneFeatureService
{
public:
  drogon::Task<std::optional<ZoneSchema>> create(const CreateZoneDto& body) const;
  drogon::Task<std::optional<ZoneSchema>> update(int64_t id,
                                                 const UpdateZoneDto& body) const;
  drogon::Task<bool> remove(int64_t id) const;

private:
  // Zones share the camera's module room.
  void emit(SyncOperation operation, const ZoneSchema& row) const;

  ZoneRepository repository_;
  CameraRepository cameraRepository_;
};
