#pragma once

#include <argus/camera/v1/sync.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>

struct SyncIdentity
{
  int64_t userId{0};
  std::string role;
  std::string device;
};

struct CameraSyncClientConfig
{
  std::string target;
  std::string credential;
};

class CameraSyncClient
{
public:
  explicit CameraSyncClient(CameraSyncClientConfig config);

  CameraSyncClient(const CameraSyncClient&) = delete;
  CameraSyncClient& operator=(const CameraSyncClient&) = delete;
  virtual ~CameraSyncClient() = default;

  virtual std::optional<argus::camera::v1::PullTableResponse> pullTable(
      const argus::camera::v1::PullTableRequest& request,
      const SyncIdentity& identity) const;

  virtual std::optional<argus::camera::v1::ListCatalogResponse>
  listCatalog(const SyncIdentity& identity) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::camera::v1::SyncService::StubInterface> stub_;
  std::string credential_;
};
