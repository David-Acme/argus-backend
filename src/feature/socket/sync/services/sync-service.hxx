#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <feature/socket/sync/services/synchronized-service.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <json/value.h>
#include <shared/services/room/room-manager.hxx>

class SyncService
{
public:
  SyncService() = default;

  drogon::Task<void> handleConnect(const drogon::HttpRequestPtr& req,
                                   const drogon::WebSocketConnectionPtr& conn) const;
  drogon::Task<void> handleMessage(const drogon::WebSocketConnectionPtr& conn,
                                   const Json::Value& obj) const;
  void handleDisconnect(const drogon::WebSocketConnectionPtr& conn) const;

private:
  SynchronizedService synchronizedService_;
  RoomManager roomManager_;
};
