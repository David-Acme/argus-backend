#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/camera/dtos/create-camera-dto.hxx>
#include <feature/api/camera/dtos/update-camera-dto.hxx>
#include <optional>
#include <sync/camera-change-sink.hxx>
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
  // Emits camera changes through the domain's sink so subscribers see them.
  [[nodiscard]] drogon::Task<void> emit(SyncOperation operation,
                                        const CameraSchema& row) const;

  CameraRepository repository_;
};
