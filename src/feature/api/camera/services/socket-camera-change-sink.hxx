#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/contracts/camera-change-sink.hxx>
#include <shared/services/socket/socket-service.hxx>
#include <shared/services/sync-audit/sync-audit-service.hxx>

// Legacy substrate of the camera-domain change events: the local SocketService
// rooms plus the SyncAuditService diff publication, exactly the pre-cutover
// path. argus-camera binds its own NATS funnel instead.
class SocketCameraChangeSink : public CameraChangeSink
{
public:
  void emitModule(TableName table, const SocketEmitDto& body) const override;
  drogon::Task<void>
  publishAudit(const CameraAuditInput& input) const override;

private:
  SocketService socketService_;
  SyncAuditService syncAuditService_;
};