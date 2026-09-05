#pragma once

#include <drogon/utils/coroutine.h>
#include <memory>
#include <shared/contracts/camera-change-sink.hxx>

class NatsBus;

// Camera-domain substrate of argus-camera (Ruling Y): change emits and audit
// diffs funnel over `argus.camera.v1.change` instead of local rooms, and
// nothing is persisted locally. The gateway inserts the audit row verbatim
// into its substrate before fanning the change out.
class NatsCameraChangeSink : public CameraChangeSink
{
public:
  explicit NatsCameraChangeSink(std::shared_ptr<NatsBus> bus);

  void emitModule(TableName table, const SocketEmitDto& body) const override;
  drogon::Task<void>
  publishAudit(const CameraAuditInput& input) const override;

private:
  std::shared_ptr<NatsBus> bus_;
};
