#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/camera/dtos/create-camera-dto.hxx>
#include <feature/api/camera/dtos/update-camera-dto.hxx>
#include <optional>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/schemas/camera/camera-schema.hxx>
#include <shared/services/socket/socket-service.hxx>
#include <shared/services/sync-audit/sync-audit-service.hxx>

class CameraFeatureService
{
public:
  drogon::Task<CameraSchema> create(const CreateCameraDto& body) const;
  drogon::Task<std::optional<CameraSchema>>
  update(int64_t id, const UpdateCameraDto& body) const;
  drogon::Task<bool> remove(int64_t id) const;

private:
  // A camera belongs to the house, not to a user, so it goes to the module
  // room: every session with read access to `camera` sees the change.
  void emit(SyncOperation operation, const CameraSchema& row) const;

  CameraRepository repository_;
  SocketService socketService_;
  SyncAuditService syncAuditService_;
};
