#pragma once

#include <drogon/WebSocketController.h>
#include <feature/socket/sync/socket/sync-forwarder.hxx>
#include <feature/socket/sync/services/sync-service.hxx>
#include <memory>
#include <shared/contracts/camera-sync-source.hxx>
#include <shared/contracts/notification-sync-source.hxx>
#include <shared/contracts/productivity-sync-source.hxx>

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
  void setUserDirectory(std::shared_ptr<const IUserDirectory> directory);

  WS_PATH_LIST_BEGIN
  WS_PATH_ADD("/sync", "DeviceFilter", "JwtFilter");
  WS_PATH_LIST_END

private:
  SyncService service_;
};
