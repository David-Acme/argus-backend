#pragma once

#include <argus/productivity/v1/sync.grpc.pb.h>
#include <grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>

// Thin SDK wrapper over argus.productivity.v1.SyncService (rule 23).
class ProductivitySyncClient
{
public:
  explicit ProductivitySyncClient(std::string target);

  ProductivitySyncClient(const ProductivitySyncClient&) = delete;
  ProductivitySyncClient& operator=(const ProductivitySyncClient&) = delete;
  virtual ~ProductivitySyncClient() = default;

  // Sync-table pull; nullopt when argus-productivity refuses or is unreachable.
  virtual std::optional<argus::productivity::v1::PullTableResponse>
  pullTable(const argus::productivity::v1::PullTableRequest& request,
            const argus::sdk::CallerIdentity& identity) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::productivity::v1::SyncService::StubInterface> stub_;
};
