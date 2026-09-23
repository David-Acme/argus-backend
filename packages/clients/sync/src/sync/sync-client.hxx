#pragma once

#include <argus/sync/v1/sync.grpc.pb.h>
#include <cstdint>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-change.hxx>
#include <sync/sync-control-sink.hxx>

struct SyncClientConfig
{
  std::string target;
  std::string fleetSecret;
};

class SyncClient : public SyncControlSink
{
public:
  explicit SyncClient(SyncClientConfig config);

  SyncClient(const SyncClient&) = delete;
  SyncClient& operator=(const SyncClient&) = delete;
  ~SyncClient() override = default;

  [[nodiscard]] bool
  replaceRoleRooms(const sync_change::RoleRoomChange& change) const override;

  [[nodiscard]] bool disconnectUser(int64_t userId,
                                    const SocketEmitDto& frame) const override;

  [[nodiscard]] bool emitToUser(int64_t userId,
                                const SocketEmitDto& frame) const override;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::sync::v1::SyncControlService::StubInterface> stub_;
  std::string fleetSecret_;
};
