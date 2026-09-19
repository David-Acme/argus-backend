#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/camera/dtos/create-camera-dto.hxx>
#include <feature/api/camera/dtos/update-camera-dto.hxx>
#include <optional>
#include <shared/contracts/camera-change-sink.hxx>
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
  // Emits camera changes to the module room so every reader session sees them.
  void emit(SyncOperation operation, const CameraSchema& row) const;

  CameraRepository repository_;
};
