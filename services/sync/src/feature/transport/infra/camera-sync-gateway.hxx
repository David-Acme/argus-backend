#pragma once

#include <camera/camera-sync-client.hxx>
#include <memory>
#include <feature/transport/infra/camera-sync-source.hxx>

// Pull source backed by argus-camera's argus.camera.v1.SyncService leg.
class CameraSyncGateway : public CameraSyncSource
{
public:
  explicit CameraSyncGateway(std::string target);

  CameraSyncGateway(const CameraSyncGateway&) = delete;
  CameraSyncGateway& operator=(const CameraSyncGateway&) = delete;

  [[nodiscard]] bool serves(CameraSyncTable table) const override;
  [[nodiscard]] std::unique_ptr<Syncable>
  sourceFor(CameraSyncTable table, const JwtContext& ctx) const override;

private:
  class Pull;

  std::shared_ptr<CameraSyncClient> client_;
};
