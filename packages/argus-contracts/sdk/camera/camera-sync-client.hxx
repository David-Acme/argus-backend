#pragma once

#include <argus/camera/v1/sync.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>

// Caller identity forwarded as x-argus-* metadata; presence is required.
struct SyncIdentity
{
  int64_t userId{0};
  std::string role;
  std::string device;
};

// Thin SDK wrapper over argus.camera.v1.SyncService (rule 23).
class CameraSyncClient
{
public:
  explicit CameraSyncClient(std::string target);

  CameraSyncClient(const CameraSyncClient&) = delete;
  CameraSyncClient& operator=(const CameraSyncClient&) = delete;
  virtual ~CameraSyncClient() = default;

  // Sync-table pull; nullopt when argus-camera refuses or is unreachable.
  virtual std::optional<argus::camera::v1::PullTableResponse>
  pullTable(const argus::camera::v1::PullTableRequest& request,
            const SyncIdentity& identity) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::camera::v1::SyncService::StubInterface> stub_;
};
