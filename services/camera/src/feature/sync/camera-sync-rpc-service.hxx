#pragma once

#include <argus/camera/v1/sync.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <feature/sync/repositories/camera-stream/camera-stream-repository.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/repositories/zone/zone-repository.hxx>

class CameraSyncRpcService final
    : public argus::camera::v1::SyncService::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  PullTable(grpc::CallbackServerContext* context,
            const argus::camera::v1::PullTableRequest* request,
            argus::camera::v1::PullTableResponse* response) override;

  grpc::ServerUnaryReactor*
  ListCatalog(grpc::CallbackServerContext* context,
              const argus::camera::v1::ListCatalogRequest* request,
              argus::camera::v1::ListCatalogResponse* response) override;

private:
  CameraRepository cameras_;
  CameraStreamRepository streams_;
  ZoneRepository zones_;
};
