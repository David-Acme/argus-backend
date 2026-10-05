#pragma once

#include <drogon/WebSocketController.h>
#include <sync/sync-forwarder.hxx>
#include <feature/transport/services/sync-service.hxx>
#include <memory>
#include <auth/user-directory.hxx>
#include <feature/transport/infra/camera-sync-source.hxx>
#include <feature/transport/infra/heartbeat-source.hxx>
#include <feature/transport/infra/identity-sync-source.hxx>
#include <feature/transport/infra/notification-sync-source.hxx>
#include <feature/transport/infra/productivity-sync-source.hxx>

class SyncSocket : public drogon::WebSocketController<SyncSocket, false>
{
public:
  void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                        std::string&& message,
                        const drogon::WebSocketMessageType& type) override;
  void handleNewConnection(const drogon::HttpRequestPtr& req,
                           const drogon::WebSocketConnectionPtr& conn) override;
  void handleConnectionClosed(
      const drogon::WebSocketConnectionPtr& conn) override;

  void setForwarder(std::shared_ptr<SyncForwarder> forwarder);
  void setCameraSource(std::shared_ptr<CameraSyncSource> source);
  void setProductivitySource(std::shared_ptr<ProductivitySyncSource> source);
  void setNotificationSource(std::shared_ptr<NotificationSyncSource> source);
  void setIdentitySource(std::shared_ptr<IdentitySyncSource> source);
  void setUserDirectory(std::shared_ptr<const IUserDirectory> directory);
  void setHeartbeatSource(std::shared_ptr<const HeartbeatSource> source);

  WS_PATH_LIST_BEGIN
  WS_PATH_ADD("/sync", "DeviceFilter", "JwtFilter");
  WS_PATH_LIST_END

private:
  SyncService service_;
};
