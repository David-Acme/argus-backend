#pragma once

#include <argus/camera/v1/sync.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <shared/repositories/camera-stream/camera-stream-repository.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/repositories/zone/zone-repository.hxx>

// argus.camera.v1.SyncService: camera-domain sync-table pulls.
class CameraSyncRpcService final
    : public argus::camera::v1::SyncService::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  PullTable(grpc::CallbackServerContext* context,
            const argus::camera::v1::PullTableRequest* request,
            argus::camera::v1::PullTableResponse* response) override;

private:
  CameraRepository cameras_;
  CameraStreamRepository streams_;
  ZoneRepository zones_;
};
