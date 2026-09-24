#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/camera/dtos/create-camera-dto.hxx>
#include <feature/camera/dtos/update-camera-dto.hxx>
#include <optional>
#include <sync/camera-change-sink.hxx>
#include <sync/module-emit.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/schemas/camera/camera-schema.hxx>

class CameraFeatureService
{
public:
  drogon::Task<CameraSchema> create(const CreateCameraDto& body) const;
  drogon::Task<std::optional<CameraSchema>>
  update(int64_t id, const UpdateCameraDto& body) const;
  drogon::Task<bool> remove(int64_t id) const;

private:
  [[nodiscard]] drogon::Task<void> emit(const ModuleEmitInput& input) const;

  CameraRepository repository_;
};
