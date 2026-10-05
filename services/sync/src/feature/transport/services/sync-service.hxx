#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <feature/transport/services/connection-lanes.hxx>
#include <feature/transport/services/frame-lane.hxx>
#include <feature/transport/services/synchronized-service.hxx>
#include <sync/sync-forwarder.hxx>
#include <auth/jwt-filter.hxx>
#include <json/value.h>
#include <memory>
#include <feature/transport/infra/camera-sync-source.hxx>
#include <feature/transport/infra/heartbeat-source.hxx>
#include <feature/transport/infra/identity-sync-source.hxx>
#include <feature/transport/infra/notification-sync-source.hxx>
#include <feature/transport/infra/productivity-sync-source.hxx>
#include <auth/user-directory.hxx>
#include <shared/services/room/room-manager.hxx>
#include <string_view>

class SyncService
{
public:
  SyncService();
  SyncService(const SyncService&) = delete;
  SyncService& operator=(const SyncService&) = delete;
  SyncService(SyncService&&) = delete;
  SyncService& operator=(SyncService&&) = delete;
  ~SyncService() = default;

  drogon::Task<void>
  handleConnect(const drogon::HttpRequestPtr& req,
                const drogon::WebSocketConnectionPtr& conn) const;
  drogon::Task<void> handleMessage(const SyncFrameInput& input) const;
  [[nodiscard]] std::shared_ptr<FrameLane>
  laneFor(const drogon::WebSocketConnectionPtr& conn) const;
  void startDrain(const drogon::WebSocketConnectionPtr& conn,
                  const std::shared_ptr<FrameLane>& lane) const;
  void handleBinary(const drogon::WebSocketConnectionPtr& conn,
                    const std::string& data) const;
  void handleDisconnect(const drogon::WebSocketConnectionPtr& conn) const;

  void setForwarder(std::shared_ptr<SyncForwarder> forwarder);
  void setCameraSource(std::shared_ptr<CameraSyncSource> source);
  void setProductivitySource(std::shared_ptr<ProductivitySyncSource> source);
  void setNotificationSource(std::shared_ptr<NotificationSyncSource> source);
  void setIdentitySource(std::shared_ptr<IdentitySyncSource> source);
  void setUserDirectory(std::shared_ptr<const IUserDirectory> directory);
  void setHeartbeatSource(std::shared_ptr<const HeartbeatSource> source);
  void setLanes(std::shared_ptr<ConnectionLanes> lanes);

private:
  void sendHeartbeat(const drogon::WebSocketConnectionPtr& conn,
                     int64_t userId) const;
  drogon::Task<void>
  refreshContext(const drogon::WebSocketConnectionPtr& conn) const;
  drogon::Task<void> runFrame(const drogon::WebSocketConnectionPtr& conn,
                              FrameJob job) const;
  drogon::Task<void>
  revalidate(const drogon::WebSocketConnectionPtr& conn) const;
  void closeDisabled(const drogon::WebSocketConnectionPtr& conn) const;

  SynchronizedService synchronizedService_;
  RoomManager roomManager_;
  std::shared_ptr<SyncForwarder> forwarder_;
  std::shared_ptr<CameraSyncSource> cameraSource_;
  std::shared_ptr<ProductivitySyncSource> productivitySource_;
  std::shared_ptr<NotificationSyncSource> notificationSource_;
  std::shared_ptr<IdentitySyncSource> identitySyncSource_;
  std::shared_ptr<const IUserDirectory> userDirectory_;
  std::shared_ptr<const HeartbeatSource> heartbeatSource_;
  std::shared_ptr<ConnectionLanes> lanes_;
};
