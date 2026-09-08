#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <feature/socket/sync/services/synchronized-service.hxx>
#include <feature/socket/sync/socket/sync-forwarder.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <json/value.h>
#include <memory>
#include <shared/contracts/camera-sync-source.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/room/room-manager.hxx>
#include <string_view>

// Serves the sync tables natively and hands every other frame to the installed forwarder.
class SyncService
{
public:
  drogon::Task<void>
  handleConnect(const drogon::HttpRequestPtr& req,
                const drogon::WebSocketConnectionPtr& conn) const;
  drogon::Task<void> handleMessage(const drogon::WebSocketConnectionPtr& conn,
                                   const Json::Value& obj,
                                   std::string_view rawMessage) const;
  void handleBinary(const drogon::WebSocketConnectionPtr& conn,
                    const std::string& data) const;
  void handleDisconnect(const drogon::WebSocketConnectionPtr& conn) const;

  void setForwarder(std::shared_ptr<SyncForwarder> forwarder);
  void setCameraSource(std::shared_ptr<CameraSyncSource> source);

private:
  drogon::Task<void>
  refreshContext(const drogon::WebSocketConnectionPtr& conn) const;

  SynchronizedService synchronizedService_;
  RoomManager roomManager_;
  UserRepository userRepository_;
  std::shared_ptr<SyncForwarder> forwarder_;
  std::shared_ptr<CameraSyncSource> cameraSource_;
};
