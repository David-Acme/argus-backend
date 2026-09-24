#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <feature/transport/services/synchronized-service.hxx>
#include <sync/sync-forwarder.hxx>
#include <auth/jwt-filter.hxx>
#include <json/value.h>
#include <memory>
#include <feature/transport/infra/camera-sync-source.hxx>
#include <feature/transport/infra/identity-sync-source.hxx>
#include <feature/transport/infra/notification-sync-source.hxx>
#include <feature/transport/infra/productivity-sync-source.hxx>
#include <auth/user-directory.hxx>
#include <shared/services/room/room-manager.hxx>
#include <string_view>

class SyncService
{
public:
  drogon::Task<void>
  handleConnect(const drogon::HttpRequestPtr& req,
                const drogon::WebSocketConnectionPtr& conn) const;
  drogon::Task<void> handleMessage(const SyncFrameInput& input) const;
  void handleBinary(const drogon::WebSocketConnectionPtr& conn,
                    const std::string& data) const;
  void handleDisconnect(const drogon::WebSocketConnectionPtr& conn) const;

  void setForwarder(std::shared_ptr<SyncForwarder> forwarder);
  void setCameraSource(std::shared_ptr<CameraSyncSource> source);
  void setProductivitySource(std::shared_ptr<ProductivitySyncSource> source);
  void setNotificationSource(std::shared_ptr<NotificationSyncSource> source);
  void setIdentitySource(std::shared_ptr<IdentitySyncSource> source);
  void setUserDirectory(std::shared_ptr<const IUserDirectory> directory);

private:
  drogon::Task<void>
  refreshContext(const drogon::WebSocketConnectionPtr& conn) const;

  SynchronizedService synchronizedService_;
  RoomManager roomManager_;
  std::shared_ptr<SyncForwarder> forwarder_;
  std::shared_ptr<CameraSyncSource> cameraSource_;
  std::shared_ptr<ProductivitySyncSource> productivitySource_;
  std::shared_ptr<NotificationSyncSource> notificationSource_;
  std::shared_ptr<IdentitySyncSource> identitySyncSource_;
  std::shared_ptr<const IUserDirectory> userDirectory_;
};
