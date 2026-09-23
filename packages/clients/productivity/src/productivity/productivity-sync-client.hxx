#pragma once

#include <argus/productivity/v1/sync.grpc.pb.h>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>

class ProductivitySyncClient
{
public:
  explicit ProductivitySyncClient(std::string target);

  ProductivitySyncClient(const ProductivitySyncClient&) = delete;
  ProductivitySyncClient& operator=(const ProductivitySyncClient&) = delete;
  virtual ~ProductivitySyncClient() = default;

  virtual std::optional<argus::productivity::v1::PullTableResponse>
  pullTable(const argus::productivity::v1::PullTableRequest& request,
            const argus::client::CallerIdentity& identity) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::productivity::v1::SyncService::StubInterface> stub_;
};
