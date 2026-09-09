#pragma once

#include <drogon/utils/coroutine.h>
#include <memory>
#include <shared/contracts/camera-change-sink.hxx>

class NatsBus;

// Camera-domain change funnel over argus.camera.v1.change.
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
