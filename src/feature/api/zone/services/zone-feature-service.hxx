#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/zone/dtos/create-zone-dto.hxx>
#include <feature/api/zone/dtos/update-zone-dto.hxx>
#include <optional>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/repositories/zone/zone-repository.hxx>
#include <shared/schemas/zone/zone-schema.hxx>
#include <shared/services/socket/socket-service.hxx>
#include <shared/services/sync-audit/sync-audit-service.hxx>

class ZoneFeatureService
{
public:
  drogon::Task<std::optional<ZoneSchema>> create(const CreateZoneDto& body) const;
  drogon::Task<std::optional<ZoneSchema>> update(int64_t id,
                                                 const UpdateZoneDto& body) const;
  drogon::Task<bool> remove(int64_t id) const;

private:
  // Zones belong to a camera, which belongs to the house: same module room as
  // the camera itself.
  void emit(SyncOperation operation, const ZoneSchema& row) const;

  ZoneRepository repository_;
  CameraRepository cameraRepository_;
  SocketService socketService_;
  SyncAuditService syncAuditService_;
};
