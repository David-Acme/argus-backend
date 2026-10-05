#pragma once

#include <argus/camera/v1/sync.grpc.pb.h>
#include <auth/user-role.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>
#include <feature/sync/repositories/camera-stream/camera-stream-repository.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/repositories/zone/zone-repository.hxx>
#include <json/value.h>
#include <vector>

namespace camera_sync_projection
{
void apply(Json::Value& row, UserRole role);
}

class CameraSyncRpcService final
    : public argus::camera::v1::SyncService::CallbackService
{
public:
  explicit CameraSyncRpcService(
      std::vector<argus::client::CallerCredential> callers);

  grpc::ServerUnaryReactor*
  PullTable(grpc::CallbackServerContext* context,
            const argus::camera::v1::PullTableRequest* request,
            argus::camera::v1::PullTableResponse* response) override;

  grpc::ServerUnaryReactor*
  ListCatalog(grpc::CallbackServerContext* context,
              const argus::camera::v1::ListCatalogRequest* request,
              argus::camera::v1::ListCatalogResponse* response) override;

private:
  std::vector<argus::client::CallerCredential> callers_;
  CameraRepository cameras_;
  CameraStreamRepository streams_;
  ZoneRepository zones_;
};
