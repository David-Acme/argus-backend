#pragma once

#include <argus/identity/v1/sync.grpc.pb.h>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>

struct IdentitySyncClientConfig
{
  std::string target;
  std::string credential{};
  std::string fleetSecret;
};

class IdentitySyncClient
{
public:
  explicit IdentitySyncClient(IdentitySyncClientConfig config);

  IdentitySyncClient(const IdentitySyncClient&) = delete;
  IdentitySyncClient& operator=(const IdentitySyncClient&) = delete;
  virtual ~IdentitySyncClient() = default;

  [[nodiscard]] virtual std::optional<argus::identity::v1::PullTableResponse>
  pullTable(const argus::identity::v1::PullTableRequest& request,
            const argus::client::CallerIdentity& identity) const;

private:
  argus::client::PeerCredential credential_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::identity::v1::SyncService::StubInterface> stub_;
};
