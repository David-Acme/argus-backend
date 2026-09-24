#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/zone/dtos/create-zone-dto.hxx>
#include <feature/zone/dtos/update-zone-dto.hxx>
#include <optional>
#include <sync/camera-change-sink.hxx>
#include <sync/module-emit.hxx>
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
  [[nodiscard]] drogon::Task<void> emit(const ModuleEmitInput& input) const;

  ZoneRepository repository_;
  CameraRepository cameraRepository_;
};
